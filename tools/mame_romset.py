#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read Model 2 ROM set layouts out of MAME's driver, and build region images.

Every Model 2 title lays its ROMs out differently: which chips make up the
i960 program, where the data ROM's mirrors go, whether the coprocessor's data
socket is filled. That table already exists and is already correct -- it is the
``ROM_START`` block in MAME's ``sega/model2.cpp``. Transcribing it by hand, once
per title, is how the two existing ports got their ``rom_loader.py``, and it does
not scale past two: 90 sets is 90 transcriptions and 90 chances to put a chip at
the wrong offset and get a region full of plausible-looking garbage.

So this reads the driver instead. Point it at a local copy of ``model2.cpp``
(``ref/model2.cpp``; MAME is a reference here, never redistributed) and it gives
you every set's board variant and region layout. The same table then drives
``build_regions``, which turns a MAME set ZIP into the flat ``program.bin`` /
``data.bin`` / ... images ``model2recomp_load_rom`` expects.

    python tools/mame_romset.py catalog          # every set, as JSON
    python tools/mame_romset.py regions vcop corpus/roms/vcop.zip out/vcop

Nothing here reads or ships ROM data of its own; it needs a set you supply.
"""

import json
import os
import re
import sys
import zipfile

# --- Which MAME region becomes which file model2recomp loads. -----------------
#
# model2recomp_load_rom() reads a directory of flat images, one per region, and
# treats everything but program.bin as optional. Regions not named here (the
# sound board's 68000 code and MultiPCM samples, the 315-5881 key) have nothing
# to load them yet; they are still catalogued, just not written.
REGION_FILES = {
    "maincpu":          "program.bin",
    "main_data":        "data.bin",
    "polygons":         "polygons.bin",
    "textures":         "textures.bin",
    "copro_tgp_tables": "copro_tables.bin",
    "copro_data":       "copro_data.bin",
}

# The driver's state class is what actually says which board a set runs on --
# the GAME() macro carries no board field. model2o is the original 1993 board,
# the rest are the CRX revisions.
BOARDS = {
    "model2o_state":         "Model 2",
    "model2o_maxx_state":    "Model 2",
    "model2o_gtx_state":     "Model 2",
    "model2a_state":         "2A-CRX",
    "model2a_airwlkrs_state": "2A-CRX",
    "model2b_state":         "2B-CRX",
    "model2c_state":         "2C-CRX",
}

_HEX = r"0x[0-9a-fA-F]+|\d+"


def _num(s):
    s = s.strip()
    return int(s, 16) if s.lower().startswith("0x") else int(s)


def _expand_macros(text):
    """Inline the two bare macros that appear inside ROM_START blocks.

    MODEL2_CPU_BOARD and MODEL2A_VID_BOARD are #defines holding whole
    ROM_REGIONs -- the coprocessor's math tables among them, which this library
    very much does load. Left unexpanded, every set that uses them looks like it
    has no math-table ROM at all.
    """
    for macro in ("MODEL2_CPU_BOARD", "MODEL2A_VID_BOARD"):
        m = re.search(r"^#define\s+%s\s*\\\n((?:.*\\\n)*.*)$" % macro, text, re.M)
        if not m:
            continue
        body = m.group(1).replace("\\\n", "\n")
        # Replace uses, not the definition itself.
        text = text[:m.start()] + text[m.start():].replace(
            "\n\t" + macro + "\n", "\n" + body + "\n")
    return text


def parse_source(path):
    """Parse model2.cpp into {setname: {...}}.

    Returns each set's description, year, parent, board variant and regions.
    A region is {size, loads: [(file, offset, length, step)], copies: [...]}
    where ``step`` is 4 for the interleaved ROM_LOAD32_WORD chips and 1 for a
    plain sequential load.
    """
    text = _expand_macros(open(path, encoding="utf-8", errors="replace").read())

    # --- GAME()/GAMEL() lines: description, year, parent, board --------------
    meta = {}
    game_re = re.compile(
        r"^GAMEL?\(\s*(\d{4})\??\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*\w+\s*,\s*\w+\s*,"
        r"\s*(\w+)\s*,\s*\w+\s*,\s*\w+\s*,\s*\"([^\"]*)\"\s*,\s*\"([^\"]*)\"",
        re.M)
    for gm in game_re.finditer(text):
        year, name, parent, state, maker, desc = gm.groups()
        meta[name] = {
            "year": int(year), "parent": ("" if parent == "0" else parent),
            "board": BOARDS.get(state, state), "maker": maker, "desc": desc,
            # MAME's own verdict. A set it cannot run is a set with no
            # reference behaviour to compare a port against, which is worth
            # knowing before spending a week on one.
            "mame_working": True,
            "protected": False,
        }
    for m in re.finditer(r"^GAMEL?\(.*?,\s*(\w+)\s*,.*$", text, re.M):
        line = m.group(0)
        if "MACHINE_NOT_WORKING" in line:
            meta.setdefault(m.group(1), {}).update(mame_working=False)
        # Sets whose data is streamed through a Sega cryptographic device (the
        # 315-5881, or the 317-0229 on Dead or Alive). Nothing downstream of
        # that chip is meaningful until it is decrypted, so the title cannot
        # draw however complete the board is - it is a blocker in its own
        # right and worth separating from "the coprocessor is missing".
        if re.search(r"5881|0229|init_(doa|zerogun|pltkids|sgt24h)", line):
            meta.setdefault(m.group(1), {}).update(protected=True)

    # --- ROM_START blocks -----------------------------------------------------
    sets = {}
    for block in re.finditer(r"^ROM_START\(\s*(\w+)\s*\)(.*?)^ROM_END", text,
                             re.M | re.S):
        name, body = block.group(1), block.group(2)
        regions, cur = {}, None
        for line in body.splitlines():
            line = line.split("//")[0].strip()

            m = re.match(r"ROM_REGION(?:32_LE|32_BE|16_BE|16_LE)?\(\s*(%s)\s*,"
                         r"\s*([\"\w]+)" % _HEX, line)
            if m:
                rname = m.group(2).strip('"')
                cur = regions.setdefault(rname, {"size": _num(m.group(1)),
                                                 "loads": [], "copies": []})
                continue
            if cur is None:
                continue

            # ROM_LOAD32_WORD(name, offset, len, ...) -- 16 bits of each 32-bit
            # word. MAME encodes which half in bit 1 of the offset.
            m = re.match(r"ROMX?_LOAD32_WORD\w*\(\s*\"([^\"]+)\"\s*,\s*(%s)\s*,"
                         r"\s*(%s)" % (_HEX, _HEX), line)
            if m:
                off = _num(m.group(2))
                cur["loads"].append([m.group(1), off & ~3, _num(m.group(3)),
                                     4, off & 2, False])
                continue

            # Plain and word-swapped sequential loads.
            m = re.match(r"ROMX?_LOAD(16_WORD_SWAP|16_BYTE|)\w*\(\s*\"([^\"]+)\""
                         r"\s*,\s*(%s)\s*,\s*(%s)" % (_HEX, _HEX), line)
            if m:
                cur["loads"].append([m.group(2), _num(m.group(3)),
                                     _num(m.group(4)), 1, 0,
                                     m.group(1) == "16_WORD_SWAP"])
                continue

            # ROM_COPY(srcregion, srcoffs, dstoffs, length). Order matters: a
            # later copy can read what an earlier one wrote, which is how
            # Daytona fills 4 MB of mirrors from a 1 MB block in three steps.
            m = re.match(r"ROM_COPY\(\s*\"?(\w+)\"?\s*,\s*(%s)\s*,\s*(%s)\s*,"
                         r"\s*(%s)" % (_HEX, _HEX, _HEX), line)
            if m:
                cur["copies"].append([m.group(1), _num(m.group(2)),
                                      _num(m.group(3)), _num(m.group(4))])

        info = dict(meta.get(name, {"year": 0, "parent": "", "board": "?",
                                    "maker": "", "desc": name,
                                    "mame_working": True}))
        info["set"] = name
        info["regions"] = regions
        sets[name] = info
    return sets


def _keys(name):
    """Every spelling a chip plausibly has in a set ZIP, most exact first.

    MAME has renamed these chips repeatedly and sets in the wild carry whatever
    spelling was current when they were built: ``mpr-16537.28`` became
    ``mpr-16537.ic28``, ``ep18066.13`` became ``epr-18066a.13``, and Desert
    Tank's ``mpr-16964`` moved socket from .21 to .20. The chip is identified by
    its part number; the suffix is which socket it sits in and the prefix is
    what kind of device it is, so falling back to the part number alone finds
    the right data when the label has drifted.

    The last key drops a trailing revision letter, which is a genuinely
    different chip rather than a different label. build_region reports anything
    matched that loosely, so a substitution is never silent.
    """
    lower = name.lower()
    stem, _, suffix = lower.rpartition(".")
    keys = [lower]
    if stem and suffix.startswith("ic"):
        keys.append("%s.%s" % (stem, suffix[2:]))
    elif stem and suffix.isdigit():
        keys.append("%s.ic%s" % (stem, suffix))
    part = re.sub(r"^(epr|mpr|opr|ep|mp|op)[-_]?", "", stem or lower)
    part = re.sub(r"[^a-z0-9]", "", part)
    if part:
        keys.append("#" + part)
        if part[-1].isalpha():
            keys.append("#" + part[:-1])
        else:
            keys.append("#" + part + "*")
    return keys


def _index(zf):
    """Map every spelling of every member to its real name."""
    idx = {}
    for i in zf.infolist():
        real = i.filename
        base = os.path.basename(real).lower()
        for k in (real.lower(), base):
            idx.setdefault(k, real)
        stem = base.rpartition(".")[0] or base
        part = re.sub(r"^(epr|mpr|opr|ep|mp|op)[-_]?", "", stem)
        part = re.sub(r"[^a-z0-9]", "", part)
        if part:
            idx.setdefault("#" + part, real)
            # A revisionless request should still find a revised chip.
            if part[-1].isalpha():
                idx.setdefault("#" + part[:-1] + "*", real)
    return idx


def _read_chip(zf, name, members):
    """Read one chip out of the set ZIP. Returns (data, matched_name)."""
    for k in _keys(name):
        if k in members:
            return zf.read(members[k]), members[k]
    return None, None


def build_region(zf, region, members, missing, substituted=None):
    """Materialise one region as a flat bytearray.

    Absent chips are appended to ``missing``; chips found only under a
    different label land in ``substituted`` as "wanted -> got", because a
    revision substitution is a thing a reader needs to see.
    """
    out = bytearray(region["size"])
    for fname, offset, length, step, half, swap in region["loads"]:
        data, got = _read_chip(zf, fname, members)
        if data is None:
            missing.append(fname)
            continue
        if substituted is not None and os.path.basename(got).lower() != fname.lower():
            substituted.append("%s -> %s" % (fname, os.path.basename(got)))
        n = min(len(data), length) & ~1
        if step == 4:
            # Each chip supplies one 16-bit half of every 32-bit word, so its
            # bytes land at stride 4. Strided slice assignment rather than a
            # per-byte loop: the data regions are 32 MB and there are thirty-odd
            # sets, which is the difference between a sweep and an afternoon.
            # Size the destination from the source slices so the two always
            # agree; an extended slice assignment of the wrong length raises.
            start = offset + half
            # Words that fit: word j occupies start+4j and start+4j+1.
            fit = max(0, (len(out) - start - 2) // 4 + 1)
            n = min(n, 2 * fit)
            lo, hi = data[0:n:2], data[1:n:2]
            out[start:start + 4 * len(lo):4] = lo
            out[start + 1:start + 1 + 4 * len(hi):4] = hi
        elif swap:
            n = min(n, (len(out) - offset) & ~1)
            out[offset:offset + n:2] = data[1:n:2]
            out[offset + 1:offset + n:2] = data[0:n:2]
        else:
            n = min(n, len(out) - offset)
            out[offset:offset + n] = data[:n]
    return out


def build_regions(sets, setname, zip_path, out_dir):
    """Turn a MAME set ZIP into the flat region images model2recomp loads.

    Returns {filename: bytes written}; raises if the set is unknown or the
    program region came out empty (a set ZIP whose chips are all named
    something else is worth failing loudly on).
    """
    if setname not in sets:
        raise KeyError("no such Model 2 set: %s" % setname)
    spec = sets[setname]
    os.makedirs(out_dir, exist_ok=True)

    written, missing, subs = {}, {}, []
    with zipfile.ZipFile(zip_path) as zf:
        members = _index(zf)
        built = {}
        for r in spec["regions"]:
            gone = []
            built[r] = build_region(zf, spec["regions"][r], members, gone,
                                    subs if r in REGION_FILES else None)
            if gone:
                missing[r] = gone

    # Copies can cross regions and must run in source order.
    for rname, region in spec["regions"].items():
        for src, soff, doff, length in region["copies"]:
            if src in built:
                built[rname][doff:doff + length] = built[src][soff:soff + length]

    # Reject before writing, not after. A merged ZIP is tried against several
    # of a title's layouts in turn, and a layout that writes a region full of
    # zeros and only then raises leaves that garbage on disk for whichever
    # layout actually won - which is how Manx TT ended up with a program ROM
    # of nothing and lifted to exactly one function.
    if not any(built.get("maincpu", b"")):
        raise RuntimeError("%s: program region is empty; chips missing: %s"
                           % (setname,
                              ", ".join(missing.get("maincpu", [])) or "none named"))

    for rname, fname in REGION_FILES.items():
        if rname not in built:
            continue
        path = os.path.join(out_dir, fname)
        with open(path, "wb") as f:
            f.write(built[rname])
        written[fname] = len(built[rname])

    return written, missing, subs


def default_source():
    return os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "ref", "model2.cpp")


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    cmd = args[0]
    sets = parse_source(default_source())

    if cmd == "catalog":
        json.dump(sets, sys.stdout, indent=1, sort_keys=True)
        return 0
    if cmd == "regions" and len(args) == 4:
        written, missing, subs = build_regions(sets, args[1], args[2], args[3])
        for f, n in sorted(written.items()):
            print("  %-18s %9d bytes" % (f, n))
        for sub in subs:
            print("  substituted %s" % sub)
        for region, chips in sorted(missing.items()):
            print("  missing from %-18s %s" % (region, ", ".join(chips)))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
