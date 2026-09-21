#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Sweep every Model 2 ROM set through the whole pipeline, and report.

There are only about thirty Model 2 titles. That is small enough to stop
treating a port as a bespoke project and start treating the set as a corpus:
run every stage over every title, and let the table say where each one is.

    python tools/corpus.py catalog      # what exists, which board, do we have it
    python tools/corpus.py extract      # set ZIP -> flat region images
    python tools/corpus.py lift         # program.bin -> C
    python tools/corpus.py build        # C -> one executable per set
    python tools/corpus.py run          # boot headless, screenshot, classify
    python tools/corpus.py discover     # harvest missed entry points, re-lift
    python tools/corpus.py report       # COMPATIBILITY.md
    python tools/corpus.py all          # every stage in order

Each stage takes an optional list of sets, so a single title can be re-run
without the other twenty-nine:

    python tools/corpus.py lift vcop daytona

Everything lands in ``corpus/`` (gitignored: it holds ROM-derived code and
region images, which this project never commits or distributes). Only the
generated reports are committed.

Stages are deliberately sequential. Recompiling and building thirty sets in
parallel is a good way to run a machine out of memory partway through and
spend the evening working out which of the failures were real.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mame_romset  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORPUS = os.path.join(ROOT, "corpus")
BUILD = os.path.join(ROOT, "build_corpus")
STATE = os.path.join(CORPUS, "state.json")

# Where set ZIPs are looked for, first match wins. Override with --roms.
ROM_DIRS = [
    r"X:\Roms\Sega Model 2\Model 2 Romset (Merged)",
    r"X:\Roms\MAME",
    os.path.join(ROOT, "corpus", "_roms"),
]

# Clones are the same program as their parent nine times out of ten, and a
# corpus of 90 sets that is really 30 titles just makes the table harder to
# read. Sweep parents; name a clone explicitly to sweep that too.
# Set-level exceptions can go here if a clone ever turns out to differ in a way
# worth tracking.


def load_state():
    if os.path.exists(STATE):
        with open(STATE, encoding="utf-8") as f:
            return json.load(f)
    return {}


def save_state(st):
    os.makedirs(CORPUS, exist_ok=True)
    with open(STATE, "w", encoding="utf-8") as f:
        json.dump(st, f, indent=1, sort_keys=True)


def find_zip(setname, rom_dirs):
    for d in rom_dirs:
        p = os.path.join(d, setname + ".zip")
        if os.path.exists(p):
            return p
    return None


def set_dir(setname):
    return os.path.join(CORPUS, setname)


def run(cmd, timeout, cwd=None, env=None, log=None):
    """Run a command, capture everything, never hang the sweep.

    Returns (code, output); code is None on timeout. A stage that hangs on one
    set must not cost the other twenty-nine.

    Output goes to a real file rather than a pipe, because the interesting
    case is the timeout. A game that spins is exactly the one whose log says
    why, and `TimeoutExpired.stdout` hands back whatever happened to be
    flushed - which for a killed process is usually nothing. Reading "did it
    boot" off an empty log makes every stuck set look like it never started.
    """
    e = dict(os.environ)
    if env:
        e.update(env)
    path = log or os.path.join(CORPUS, "_last.log")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    code = -1
    with open(path, "wb") as f:
        try:
            p = subprocess.Popen(cmd, cwd=cwd, env=e, stdout=f,
                                 stderr=subprocess.STDOUT)
        except OSError as ex:
            return -1, str(ex)
        try:
            code = p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
            code = None
    with open(path, "rb") as f:
        return code, f.read().decode("utf-8", "replace")


# --- stages -------------------------------------------------------------------

def stage_catalog(sets, args, st):
    """Every set MAME knows, which board it is, and whether we hold the ZIP."""
    cat = mame_romset.parse_source(args.source)
    for name in sets:
        spec = cat[name]
        zp = find_zip(name, args.roms)
        st.setdefault(name, {}).update(
            desc=spec["desc"], year=spec["year"], board=spec["board"],
            maker=spec["maker"], parent=spec["parent"],
            mame_working=spec["mame_working"],
            protected=spec.get("protected", False),
            program_kb=spec["regions"].get("maincpu", {}).get("size", 0) // 1024,
            copro_data=bool(spec["regions"].get("copro_data", {}).get("loads")),
            have_rom=bool(zp), rom_path=zp or "")
        print("  %-12s %-9s %-46s %s" %
              (name, spec["board"], spec["desc"][:46],
               "have" if zp else "NO ROM"))
    return st


def stage_extract(sets, args, st):
    """Set ZIP -> the flat region images model2recomp_load_rom reads."""
    cat = mame_romset.parse_source(args.source)
    for name in sets:
        rec = st.setdefault(name, {})
        zp = rec.get("rom_path") or find_zip(name, args.roms)
        if not zp:
            rec["extract"] = "no-rom"
            print("  %-12s no ROM" % name)
            continue
        out = os.path.join(set_dir(name), "roms")

        # Merged sets carry whichever revision the collection happened to
        # have, and often under whatever chip names were current when the set
        # was built. So try this title's whole family and keep the layout the
        # ZIP satisfies most completely, rather than the first one that does
        # not throw: X's doa.zip holds doab's chips, and picking "the parent,
        # partially" over "a clone, entirely" is how a region ends up half
        # full of zeros.
        layouts = [name] + sorted(n for n, s in cat.items()
                                  if s.get("parent") == name)
        best, last, built_as = None, None, None
        for layout in layouts:
            try:
                written, missing, subs = mame_romset.build_regions(
                    cat, layout, zp, out)
            except Exception as ex:                    # noqa: BLE001
                last = ex
                continue
            built_as = layout
            # Only regions this library loads count towards "complete"; a
            # missing MultiPCM sample is not a reason to flag a set when
            # nothing here plays samples yet.
            gaps = {r: c for r, c in missing.items()
                    if r in mame_romset.REGION_FILES}
            # Rank: a gap in the program ROM is disqualifying, then fewest
            # gaps, then fewest substituted chips.
            score = (bool(gaps.get("maincpu")), len(gaps), len(subs))
            if best is None or score < best[0]:
                best = (score, layout, written, gaps, subs)
            if score == (False, 0, 0):
                break

        if best is None:
            rec["extract"] = "fail"
            rec["extract_error"] = str(last)[:200]
            print("  %-12s FAIL %s" % (name, last))
            continue

        _, layout, written, gaps, subs = best
        # The winning layout is not necessarily the one left on disk.
        if layout != built_as:
            mame_romset.build_regions(cat, layout, zp, out)
        rec["extract"] = "ok" if not gaps else "partial"
        rec.pop("extract_error", None)
        rec["layout"] = layout
        rec["missing"] = {r: c[:6] for r, c in gaps.items()}
        rec["substituted"] = subs[:8]
        rec["regions"] = sorted(written)
        print("  %-12s %-8s %d regions%s%s%s" % (
            name, rec["extract"], len(written),
            "" if layout == name else "  (as %s)" % layout,
            "  gaps: %s" % ",".join(sorted(gaps)) if gaps else "",
            "  subs: %d" % len(subs) if subs else ""))
    return st


def stage_lift(sets, args, st):
    """program.bin -> C, via the shared i960 lifter."""
    lifter = os.path.join(ROOT, "tools", "i960_lifter.py")
    for name in sets:
        rec = st.setdefault(name, {})
        prog = os.path.join(set_dir(name), "roms", "program.bin")
        if not os.path.exists(prog):
            rec["lift"] = "no-rom"
            print("  %-12s no program.bin" % name)
            continue
        out = os.path.join(set_dir(name), "recomp")
        os.makedirs(out, exist_ok=True)
        t0 = time.time()
        cmd = [sys.executable, lifter, prog, out, "game"]
        hints = hints_path(name)
        if os.path.exists(hints):
            cmd += ["--hints", hints]
        code, log = run(cmd, timeout=args.lift_timeout, cwd=ROOT,
                        log=os.path.join(set_dir(name), "lift.log"))
        funcs = 0
        for line in log.splitlines():
            if line.startswith("Found ") and "functions" in line:
                funcs = int(line.split()[1])
        rec["lift"] = ("ok" if code == 0 else
                       "timeout" if code is None else "fail")
        rec["funcs"] = funcs
        rec["lift_secs"] = round(time.time() - t0, 1)
        rec.pop("lift_error", None)
        if code != 0:
            rec["lift_error"] = (log.strip().splitlines() or [""])[-1][:200]

        # The generic launcher includes "game/functions.h"; the lifter emits
        # code against it but not the header itself.
        inc = os.path.join(set_dir(name), "include", "game")
        os.makedirs(inc, exist_ok=True)
        with open(os.path.join(inc, "functions.h"), "w", encoding="utf-8") as f:
            f.write('/* Generated by tools/corpus.py. */\n'
                    '#ifndef GAME_FUNCTIONS_H\n#define GAME_FUNCTIONS_H\n'
                    '#include "model2recomp/i960.h"\n'
                    '#include "model2recomp/bus.h"\n'
                    '#include "model2recomp/func_table.h"\n'
                    'void game_register_all(void);\n'
                    '#endif\n')
        print("  %-12s %-8s %5d funcs  %5.1fs%s" % (
            name, rec["lift"], funcs, rec["lift_secs"],
            "  " + rec.get("lift_error", "") if code != 0 else ""))
    return st


def _configure(args):
    os.makedirs(BUILD, exist_ok=True)
    if os.path.exists(os.path.join(BUILD, "CMakeCache.txt")) and not args.reconfigure:
        return
    cmd = ["cmake", "-S", ROOT, "-B", BUILD,
           "-DMODEL2RECOMP_BUILD_EXAMPLES=OFF",
           "-DMODEL2RECOMP_BUILD_TESTS=OFF",
           "-DMODEL2RECOMP_CORPUS_DIR=" + CORPUS]
    if args.toolchain:
        cmd.append("-DCMAKE_TOOLCHAIN_FILE=" + args.toolchain)
    if args.triplet:
        cmd.append("-DVCPKG_TARGET_TRIPLET=" + args.triplet)
    if os.name == "nt":
        # The 32-bit hosted compiler runs out of address space on the larger
        # lifted translation units ("C1060: out of heap space"); the 64-bit
        # host compiles the same files without trouble.
        cmd += ["-T", "host=x64"]
    code, log = run(cmd, timeout=600)
    if code != 0:
        print(log[-3000:])
        raise SystemExit("cmake configure failed")


def stage_build(sets, args, st):
    """One executable per set, sharing the generic launcher."""
    _configure(args)
    for name in sets:
        rec = st.setdefault(name, {})
        if not os.path.exists(os.path.join(set_dir(name), "recomp",
                                           "game_register.c")):
            rec["build"] = "no-code"
            print("  %-12s nothing lifted" % name)
            continue
        t0 = time.time()
        # One target at a time. Thirty lifted programs built in parallel is
        # how a machine runs out of memory halfway through a sweep.
        cmd = ["cmake", "--build", BUILD, "--config", "Release",
               "--target", "corpus_" + name]
        if os.name == "nt":
            # MSBuild keeps worker nodes alive for reuse; over thirty
            # invocations they accumulate until cl cannot load c2.dll.
            cmd += ["--", "/nodeReuse:false"]
        code, log = run(cmd, timeout=args.build_timeout)
        exe = corpus_exe(name)
        rec["build"] = "ok" if exe else ("timeout" if code is None else "fail")
        rec["build_secs"] = round(time.time() - t0, 1)
        rec.pop("build_error", None)
        if not exe:
            errs = [l for l in log.splitlines() if ": error" in l or "LNK" in l]
            rec["build_error"] = (errs[0].strip()[:200] if errs else
                                  log.strip().splitlines()[-1][:200] if log else "")
        print("  %-12s %-8s %6.1fs %s" % (name, rec["build"], rec["build_secs"],
                                          rec.get("build_error", "")))
    return st


def corpus_exe(name):
    for p in (os.path.join(BUILD, "corpus", "corpus_%s.exe" % name),
              os.path.join(BUILD, "corpus", "corpus_%s" % name)):
        if os.path.exists(p):
            return p
    return None


# model2_variant_t, as the launcher's fourth argument.
BOARD_ARG = {"Model 2": 0, "2A-CRX": 1, "2B-CRX": 2, "2C-CRX": 3}

PIXEL_STRIDE = 3 * 11          # every 11th pixel, on a pixel boundary


def ppm_pixels(path):
    """(non-zero subpixel count, distinct colours, coarse pixel sample).

    Reading the pixels beats sizing the file: a PPM is uncompressed, so its
    size says nothing at all about whether anything was drawn. The sample steps
    whole pixels rather than bytes, so a colour is a colour and not three
    unrelated channel values, and it is what the change and structure measures
    both run on.
    """
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return 0, 0, b""
    if not data.startswith(b"P6"):
        return 0, 0, b""
    fields, i = [], 2
    while len(fields) < 3 and i < len(data):
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    body = data[i + 1:]
    sample = body[::PIXEL_STRIDE // 3 * 3]
    # Distinct colours in the sample. One colour is a cleared framebuffer with
    # a background in it and nothing drawn on top - Sonic Championship spends
    # its whole run on a flat blue screen, which counts every subpixel as
    # non-zero and read as a rendered picture until this was measured. Two is
    # enough to be a picture: Gunblade's warning screen is white text on black.
    colours = len({sample[k:k + 3] for k in range(0, len(sample) - 2, 3)})
    return sum(1 for b in body if b), colours, sample


def change_fraction(a, b):
    """Fraction of sampled subpixels that differ by more than noise.

    A game sitting on one screen with a flashing "INSERT COIN" changes a few
    per cent of the picture; a game cycling its attract sequence changes most
    of it. Counting the difference rather than hashing the frame is what keeps
    the two apart, and "still reaching new state" is the whole question this
    sweep exists to answer.
    """
    n = min(len(a), len(b))
    if not n:
        return 0.0
    return sum(1 for i in range(n) if abs(a[i] - b[i]) > 8) / n


def stage_run(sets, args, st):
    """Boot each set headless, sample the screen, and classify how far it got."""
    env = {
        "SDL_VIDEODRIVER": "dummy",   # no window; the software renderer draws
        "SDL_AUDIODRIVER": "dummy",
        "MODEL2_FAST": "1",           # no vsync, no wall-clock field gate
        "MODEL2_MAX_FRAMES": str(args.frames),
        "MODEL2_SHOT_EVERY": str(args.shot_every),
        # No input by default, deliberately. Attract mode is what these games
        # do *before* a credit, so a coin is the one thing that ends the state
        # this sweep is measuring. Pass --input coin1,start1 when the question
        # is whether a title gets past the title screen instead.
        "MODEL2_INPUT": args.input,
    }
    for name in sets:
        rec = st.setdefault(name, {})
        exe = corpus_exe(name)
        roms = os.path.join(set_dir(name), "roms")
        if not exe:
            rec["run"] = "no-exe"
            print("  %-12s not built" % name)
            continue
        shots = os.path.join(set_dir(name), "shots")
        shutil.rmtree(shots, ignore_errors=True)
        os.makedirs(shots, exist_ok=True)
        e = dict(env, MODEL2_SCREENSHOT=os.path.join(shots, "f"))
        t0 = time.time()
        code, log = run([exe, roms, rec.get("desc", name),
                         str(BOARD_ARG.get(rec.get("board"), 0))],
                        timeout=args.run_timeout, env=e,
                        log=os.path.join(set_dir(name), "run.log"))

        frames = sorted(f for f in os.listdir(shots) if f.endswith(".ppm"))
        best, colours, samples = 0, 1, []
        for f in frames:
            nz, cols, px = ppm_pixels(os.path.join(shots, f))
            best = max(best, nz)
            colours = max(colours, cols)
            # Field 0 is captured before the guest has drawn anything. Counting
            # it would make every game that ever puts a pixel up look like it
            # is cycling its attract sequence.
            if not f.endswith(".00000.ppm"):
                samples.append(px)
        # Pixels alone are not a picture. A framebuffer cleared to a background
        # colour has every subpixel non-zero and nothing on it.
        lit = best > args.blank_threshold
        drew = lit and colours > 1
        churn = max((change_fraction(samples[i], samples[i + 1])
                     for i in range(len(samples) - 1)), default=0.0)

        booted = "[corpus] boot 0" in log and "not registered" not in log
        # Read the hint count off the file rather than trusting what discover
        # last wrote into the state: a rolled-back round changes the file, and
        # a table that disagrees with the tree is worse than no table.
        rec["hints"] = sum(
            1 for line in open(hints_path(name), encoding="utf-8")
            if line.startswith("entry ")) if os.path.exists(hints_path(name)) else 0
        rec.update(
            run=("timeout" if code is None else "ok" if code == 0 else "crash"),
            run_secs=round(time.time() - t0, 1),
            frames=len(frames), pixels=best, colours=colours,
            churn=round(churn, 3),
            boots="yes" if booted else "no",
            renders="yes" if drew else "no",
            motion=("advancing" if drew and churn >= args.churn else
                    "twitching" if drew and churn > 0 else
                    "frozen" if drew else "blank"),
        )
        rec["cat"] = ("ATTRACT" if drew and churn >= args.churn else
                      "STATIC" if drew else
                      "FLAT-FILL" if lit else
                      "BOOTS-BLANK" if booted and len(frames) else
                      "NO-FRAMES" if booted else "NO-BOOT")
        rec.pop("run_error", None)
        if not booted:
            bad = [l for l in log.splitlines() if "not registered" in l or
                   "failed" in l.lower()]
            rec["run_error"] = (bad[0].strip()[:200] if bad else
                                (log.strip().splitlines() or [""])[-1][:200])
        print("  %-12s %-12s frames=%-3d px=%-8d col=%-5d churn=%-5.1f%% "
              "%5.1fs %s" % (name, rec["cat"], len(frames), best, colours,
                             churn * 100, rec["run_secs"],
                             rec.get("run_error", "")))
    return st


# --- report -------------------------------------------------------------------

STAGE_COLS = [
    ("have_rom",  "ROM",      lambda r: "yes" if r.get("have_rom") else "no"),
    ("extract",   "Extract",  lambda r: r.get("extract", "-")),
    ("lift",      "Lifts",    lambda r: r.get("lift", "-")),
    ("build",     "Builds",   lambda r: r.get("build", "-")),
    ("boots",     "Boots",    lambda r: r.get("boots", "-")),
    ("renders",   "Renders",  lambda r: r.get("renders", "-")),
    ("motion",    "Motion",   lambda r: r.get("motion", "-")),
    ("cat",       "Furthest", lambda r: r.get("cat", "-")),
]

# What each verdict means, spelled out once rather than in every reader's head.
LEGEND = [
    ("`ATTRACT`", "drew a picture, and at least a tenth of it changed between "
                  "samples - the attract sequence is running"),
    ("`STATIC`", "drew a picture and stayed on it"),
    ("`FLAT-FILL`", "lit every pixel but only one colour: a framebuffer cleared "
                    "to a background with nothing drawn on it"),
    ("`BOOTS-BLANK`", "reached the field boundary, drew nothing"),
    ("`NO-FRAMES`", "never reached the first sample field before the timeout"),
    ("`NO-BOOT`", "the reset vector did not dispatch"),
]


# What each set is actually waiting on, where the sweep established it rather
# than guessed. A row with no entry here has not been diagnosed - that is the
# work queue, and it is deliberately short because each line cost a trace.
BLOCKER = {
    "daytona":  "Coprocessor handshake: the game submits a TGP job and spins "
                "on a result that never arrives.",

    "von":      "Colour ramps only partly written - 2 of 32 per channel.",
    "vstriker": "Sound board. Sits on \"SOUND initialize...\", writes the "
                "serial data register once and waits for a reply. Needs the "
                "68000; a permanently-ready transmitter is not enough.",
    "srallyc":  "Never reaches the first sample field.",
    "overrev":  "Bit-bangs the 93C46 serial EEPROM hard - 9,788 port A writes "
                "in 300 fields - and its read count is identical with no "
                "EEPROM, a read-only one and a full one, so the loop does not "
                "branch on what the device says.",
    "skisuprg": "Drew a partial screen while port A of the 315-5649 wrongly "
                "returned the cabinet inputs; blank now that it returns what "
                "the hardware does. Was relying on the inaccuracy.",
}

BOARD_NOTE = {
    "Model 2":
        "The original 1993 board — the one this library implements. These "
        "results are the library's own.",
    "2A-CRX":
        "Same MB86233 coprocessor, same geometry engine and rasterizer; a "
        "different I/O chip (Sega 315-5649 in place of the Model 1 dual-port "
        "RAM) and a different program-RAM map. A memory-map job rather than a "
        "DSP one, and the largest single win available: nine titles.",
    "2B-CRX":
        "Needs an ADSP-21062 SHARC for the math coprocessor - but that blocks "
        "3D, not attract mode. Gunblade NY reaches attract here drawing "
        "nothing but tilemaps, so a blank 2B set is stopped by something else "
        "and the SHARC is not what would fix it.",
    "2C-CRX":
        "Needs an MB86235 \"TGPx4\" for 3D. Same caveat as 2B: the tilemap "
        "and text path is shared and works, so a blank set is not waiting on "
        "the coprocessor.",
}


def stage_report(sets, args, st):
    order = ["Model 2", "2A-CRX", "2B-CRX", "2C-CRX"]
    rows = [(n, st[n]) for n in sets if n in st]

    def count(pred):
        return sum(1 for _, r in rows if pred(r))

    md = ["# Model 2 corpus",
          "",
          "Every Model 2 set MAME knows, run through the same pipeline: read the",
          "ROM layout out of the driver, build the region images, lift the i960",
          "program to C, build it against this library with the generic launcher,",
          "and boot it headless. Regenerate the whole table with",
          "`python tools/corpus.py all`.",
          "",
          "No ROM data, and no code lifted from one, is committed here. The table",
          "records what happened on a machine that had the sets.",
          "",
          "| | |", "|---|---|",
          "| Sets in the sweep | **%d** |" % len(rows),
          "| ROM set on hand | **%d** |" % count(lambda r: r.get("have_rom")),
          "| Region images built | **%d** |" % count(lambda r: r.get("extract") in ("ok", "partial")),
          "| Program lifts to C | **%d** |" % count(lambda r: r.get("lift") == "ok"),
          "| That C builds | **%d** |" % count(lambda r: r.get("build") == "ok"),
          "| Boots | **%d** |" % count(lambda r: r.get("boots") == "yes"),
          "| Draws something | **%d** |" % count(lambda r: r.get("renders") == "yes"),
          "| Reaches attract (still changing at the last sample) | **%d** |"
          % count(lambda r: r.get("motion") == "advancing"),
          "| Blocked on a Sega crypto device regardless of the board | **%d** |"
          % count(lambda r: r.get("protected")),
          "",
          "Read the board heading before the result. The memory map is now "
          "variant-aware, so the CRX boards get their own program RAM, I/O "
          "chip and texture windows - but 2B and 2C still have no math "
          "coprocessor, and without one no polygon can be transformed. A CRX "
          "title that draws only tilemaps and text is at the ceiling of what "
          "this library can currently give it, not failing. See "
          "[porting-targets.md](docs/technical/porting-targets.md).",
          "",
          "**Furthest** is how far the set got:", ""]
    md += ["- %s — %s" % (k, v) for k, v in LEGEND]
    md += ["",
           "**Hints** counts the entry points `corpus.py discover` harvested "
           "from the runtime's own dispatch misses: addresses the game computes "
           "at run time, which static analysis cannot see.",
           ""]

    hdr = "| Set | Game | Year | " + " | ".join(c[1] for c in STAGE_COLS) + \
          " | Funcs | Hints | Notes |"
    sep = "|" + "---|" * (len(STAGE_COLS) + 6)
    for board in order:
        group = [(n, r) for n, r in rows if r.get("board") == board]
        if not group:
            continue
        md += ["## %s (%d)" % (board, len(group)), "", BOARD_NOTE.get(board, ""),
               "", hdr, sep]
        for n, r in sorted(group, key=lambda x: (x[1].get("year", 0), x[0])):
            note = (BLOCKER.get(n) or r.get("build_error") or
                    r.get("lift_error") or r.get("run_error") or
                    r.get("extract_error") or "")
            if r.get("protected"):
                note = ("Behind a Sega 315-5881/317-0229 cryptographic device; "
                        "the data it streams is not decrypted here. " + note).strip()
            if not r.get("mame_working", True):
                note = ("MAME cannot run this set either. " + note).strip()
            md.append("| `%s` | %s | %s | %s | %d | %d | %s |" % (
                n, r.get("desc", n), r.get("year", ""),
                " | ".join(c[2](r) for c in STAGE_COLS),
                r.get("funcs", 0), r.get("hints", 0),
                note.replace("|", "\\|")[:120]))
        md.append("")

    out = os.path.join(ROOT, "CORPUS.md")
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(md))
    print("  wrote %s (%d sets)" % (out, len(rows)))
    return st


def hints_path(name):
    return os.path.join(set_dir(name), "hints.txt")


MISS_RE = re.compile(r"no function at 0x([0-9A-Fa-f]{8})")

# How far a set got, as a number, so two runs can be compared. Colours before
# frames: a picture with more in it beats one with more samples of nothing.
CAT_RANK_NAME = ["NO-BOOT", "NO-FRAMES", "BOOTS-BLANK", "FLAT-FILL",
                 "STATIC", "ATTRACT"]
CAT_RANK = {c: i for i, c in enumerate(CAT_RANK_NAME)}


def quality(rec):
    return (CAT_RANK.get(rec.get("cat"), 0), rec.get("colours", 0),
            rec.get("frames", 0))


def revert(path, contents):
    """Put a hints file back the way it was before a round that cost more than
    it bought."""
    if contents:
        with open(path, "w", encoding="utf-8") as f:
            f.write(contents)
    elif os.path.exists(path):
        os.remove(path)

HINTS_HEADER = """\
# Entry points %s reaches at run time, harvested from the runtime's dispatch
# misses by tools/corpus.py discover. Static analysis cannot see a target the
# game computes, so these are the addresses the running game proved it needs
# and nothing in the ROM names.
"""


def stage_discover(sets, args, st):
    """Profile-guided function discovery: run, harvest what it could not reach.

    Static analysis cannot find a jump target the game computes at run time, so
    func_table has nothing to dispatch to and the call silently does nothing.
    Air Walkers spends its whole run calling one address that was never lifted.

    But the runtime already *names* those addresses - it prints "no function at
    0x..." every time it misses. Feeding them back as entry hints makes the
    lifter emit them, which lets the game reach code that computes the next set
    of targets. So: run, harvest, re-lift, rebuild, repeat, until nothing new
    appears or the rounds run out.

    Hints accumulate in corpus/<set>/hints.txt. They are a property of the
    game, not of one run.

    A round that makes things worse is rolled back. Registering an address as
    an entry point *splits* the function containing it, and a harvested address
    is not always a function start - it can be a computed jump into the middle
    of one, or the tail of a garbage dispatch. Desert Tank was rendering its
    attract sequence, took nine harvested entries, and dropped to a static
    screen. So each round is measured, and hints that cost more than they buy
    do not survive it.
    """
    for name in sets:
        rec = st.setdefault(name, {})
        if not corpus_exe(name):
            print("  %-12s not built" % name)
            continue
        path = hints_path(name)
        known = set()
        if os.path.exists(path):
            for line in open(path, encoding="utf-8"):
                bits = line.split("#")[0].split()
                if len(bits) == 2 and bits[0] == "entry":
                    known.add(bits[1].upper())

        st = stage_run([name], args, st)
        best = quality(st[name])
        for rnd in range(1, args.rounds + 1):
            log = os.path.join(set_dir(name), "run.log")
            found = set()
            try:
                for line in open(log, encoding="utf-8", errors="replace"):
                    m = MISS_RE.search(line)
                    if m:
                        found.add(m.group(1).upper())
            except OSError:
                pass
            fresh = sorted(found - known)
            print("      round %d: %-12s %d new entr%s" %
                  (rnd, rec.get("cat", "?"), len(fresh),
                   "y" if len(fresh) == 1 else "ies"))
            if not fresh:
                break

            before = open(path, encoding="utf-8").read() if os.path.exists(path) else ""
            with open(path, "a", encoding="utf-8") as f:
                if f.tell() == 0:
                    f.write(HINTS_HEADER % name)
                for a in fresh:
                    f.write("entry %s\n" % a)
            st = stage_lift([name], args, st)
            st = stage_build([name], args, st)
            if st[name].get("build") != "ok":
                print("      build failed with the new entries; rolling back")
                revert(path, before)
                st = stage_lift([name], args, st)
                st = stage_build([name], args, st)
                break

            st = stage_run([name], args, st)
            now = quality(st[name])
            if now < best:
                print("      round %d made it worse (%s -> %s); rolling back" %
                      (rnd, CAT_RANK_NAME[best[0]], CAT_RANK_NAME[now[0]]))
                revert(path, before)
                st = stage_lift([name], args, st)
                st = stage_build([name], args, st)
                st = stage_run([name], args, st)
                break
            best = now
            known |= set(fresh)
        rec["hints"] = len(known)
    return st


STAGES = {"catalog": stage_catalog, "extract": stage_extract,
          "lift": stage_lift, "build": stage_build, "run": stage_run,
          "discover": stage_discover,
          "report": stage_report}
ALL = ["catalog", "extract", "lift", "build", "run", "report"]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stage", choices=list(STAGES) + ["all"])
    ap.add_argument("sets", nargs="*", help="sets to process (default: all parents)")
    ap.add_argument("--source", default=mame_romset.default_source())
    ap.add_argument("--roms", action="append", default=None,
                    help="directory to look for set ZIPs in (repeatable)")
    ap.add_argument("--clones", action="store_true",
                    help="include clone sets, not just parents")
    ap.add_argument("--frames", type=int, default=3600)
    ap.add_argument("--shot-every", type=int, default=600)
    ap.add_argument("--input", default="",
                    help="MODEL2_INPUT script; empty (the default) surveys attract mode")
    ap.add_argument("--churn", type=float, default=0.10,
                    help="fraction of the picture that must change between "
                         "samples to count as an attract cycle rather than a "
                         "static screen")
    ap.add_argument("--blank-threshold", type=int, default=2000,
                    help="non-zero subpixels above which a frame counts as drawn")
    ap.add_argument("--lift-timeout", type=int, default=900)
    ap.add_argument("--build-timeout", type=int, default=1800)
        # A game that is progressing gets through 3600 fields in about a minute
    # under MODEL2_FAST; one that is spinning runs until it is stopped.
    # Whatever it drew before the cap is still on disk and still measured, so
    # the timeout costs a stuck set nothing but wall clock.
    ap.add_argument("--run-timeout", type=int, default=180)
    ap.add_argument("--toolchain", default=os.environ.get("CMAKE_TOOLCHAIN_FILE"))
    ap.add_argument("--triplet", default=os.environ.get("VCPKG_TARGET_TRIPLET"))
    ap.add_argument("--rounds", type=int, default=6,
                    help="discover: how many harvest/re-lift passes")
    ap.add_argument("--reconfigure", action="store_true")
    args = ap.parse_args()
    if args.roms is None:
        args.roms = ROM_DIRS

    cat = mame_romset.parse_source(args.source)
    if args.sets:
        sets = args.sets
        unknown = [s for s in sets if s not in cat]
        if unknown:
            raise SystemExit("unknown set(s): %s" % ", ".join(unknown))
    else:
        sets = sorted(n for n, s in cat.items()
                      if args.clones or not s.get("parent"))

    st = load_state()
    for stage in (ALL if args.stage == "all" else [args.stage]):
        print("== %s (%d sets) ==" % (stage, len(sets)))
        st = STAGES[stage](sets, args, st)
        save_state(st)
    return 0


if __name__ == "__main__":
    sys.exit(main())
