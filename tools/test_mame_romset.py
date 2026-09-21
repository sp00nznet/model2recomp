#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Self-check for the MAME ROM set reader.

Runs against ``ref/model2.cpp`` if it is there (it is local scratch, not
committed), and against synthetic data otherwise, so it is useful either way.

    python tools/test_mame_romset.py
"""

import io
import os
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mame_romset as M  # noqa: E402

fails = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        fails.append(what)


def test_interleave():
    """Two chips, each 16 bits of every 32-bit word, must come back woven."""
    lo = bytes([0x11, 0x22, 0x33, 0x44])       # words 0 and 1, low halves
    hi = bytes([0xAA, 0xBB, 0xCC, 0xDD])       # ... high halves
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        zf.writestr("mpr-1.16", lo)
        zf.writestr("mpr-2.ic17", hi)
    zf = zipfile.ZipFile(buf)
    region = {"size": 8, "copies": [],
              "loads": [["mpr-1.16", 0, 4, 4, 0, False],
                        ["mpr-2.17", 0, 4, 4, 2, False]]}
    got = bytes(M.build_region(zf, region, M._index(zf), []))
    check(got == bytes([0x11, 0x22, 0xAA, 0xBB, 0x33, 0x44, 0xCC, 0xDD]),
          "ROM_LOAD32_WORD interleaves low and high halves (got %s)" % got.hex())

    # A chip that exactly fills its region must not lose its last word to an
    # off-by-one in the destination-slice length.
    region = {"size": 8, "copies": [],
              "loads": [["mpr-2.17", 0, 4, 4, 2, False]]}
    got = bytes(M.build_region(zf, region, M._index(zf), []))
    check(got[6:8] == b"\xCC\xDD", "the last word of a full region survives")

    # ROM_LOAD16_WORD_SWAP swaps each pair.
    with zipfile.ZipFile(buf, "a") as z2:
        z2.writestr("epr-3.7", bytes([1, 2, 3, 4]))
    zf = zipfile.ZipFile(buf)
    region = {"size": 4, "copies": [],
              "loads": [["epr-3.7", 0, 4, 1, 0, True]]}
    got = bytes(M.build_region(zf, region, M._index(zf), []))
    check(got == bytes([2, 1, 4, 3]), "ROM_LOAD16_WORD_SWAP swaps each pair")


def test_name_drift():
    """The chip is the part number; the label around it drifts between dumps."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        zf.writestr("mpr-16537.28", b"\x01\x02")     # MAME now says .ic28
        zf.writestr("ep18068a.15", b"\x03\x04")      # MAME now says epr-18068a
        zf.writestr("mpr-16964.21", b"\x05\x06")     # MAME now says .20
    zf = zipfile.ZipFile(buf)
    idx = M._index(zf)
    for want, expect in (("mpr-16537.ic28", "mpr-16537.28"),
                         ("epr-18068a.15", "ep18068a.15"),
                         ("mpr-16964.20", "mpr-16964.21")):
        _, got = M._read_chip(zf, want, idx)
        check(got == expect, "%s resolves to %s (got %s)" % (want, expect, got))
    _, got = M._read_chip(zf, "mpr-99999.1", idx)
    check(got is None, "a chip that really is absent stays absent")


def test_driver():
    src = M.default_source()
    if not os.path.exists(src):
        print("  skip  ref/model2.cpp not present (local scratch)")
        return
    sets = M.parse_source(src)
    check(len(sets) > 80, "parsed %d sets from the driver" % len(sets))
    check(sets["vcop"]["board"] == "Model 2", "vcop is a Model 2 original")
    check(sets["srallyc"]["board"] == "2A-CRX", "srallyc is 2A-CRX")
    check(sets["hotd"]["board"] == "2C-CRX", "hotd is 2C-CRX")
    check(not sets["srallyc"]["mame_working"],
          "srallyc is flagged MACHINE_NOT_WORKING")

    vcop = sets["vcop"]["regions"]
    check(vcop["maincpu"]["size"] == 0x200000, "vcop program region is 2 MB")
    check(len(vcop["maincpu"]["loads"]) == 4, "vcop program is four chips")
    # The coprocessor's math tables come from a macro; unexpanded, every set
    # that uses one looks like it has no table ROM at all.
    check("copro_tgp_tables" in vcop, "MODEL2_CPU_BOARD expanded")
    check(not vcop["copro_data"]["loads"], "vcop's copro data socket is empty")
    check(len(sets["daytona"]["regions"]["copro_data"]["loads"]) == 2,
          "daytona's copro data socket is filled")
    check(len(sets["daytona"]["regions"]["main_data"]["copies"]) == 7,
          "daytona's data ROM mirrors are picked up")


def main():
    print("mame_romset self-check")
    test_interleave()
    test_name_drift()
    test_driver()
    print("%d failed" % len(fails) if fails else "all checks passed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
