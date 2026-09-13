# Debugging a Model 2 Recomp

A recompiled game has no debugger stop, no single step and no disassembly view
at runtime — the guest *is* your program. What it does have is a small set of
environment variables and one technique that answers most questions.

## Environment variables

| Variable | Effect |
|---|---|
| `MODEL2_TRACE=N` | Print the first N function dispatches, indented by call depth |
| `MODEL2_SCREENSHOT=path` | Write the final frame as a binary PPM when the frame limit is reached |
| `MODEL2_SHOT_EVERY=N` | With `MODEL2_SCREENSHOT` set, also write `path.<field>.ppm` every N fields |
| `MODEL2_INPUT=a,b,...` | Drive buttons for a headless run — `coin1`, `coin2`, `start1`, `start2`, `service`, `fire`, `reload`, `test`. Each is **pulsed** (6 fields down, 54 up, staggered), because coins and start are edge triggered and a held button gives one edge and then nothing. `test` is a switch and is held. |
| `MODEL2_POLYCOUNT=N` | Every N fields, print how many polygons the geometry engine produced. Zero means the game is not submitting a display list — a different problem from one that draws nothing. |
| `MODEL2_WATCH=0xADDR` | Print every 32-bit write to that address, with the value, the guest function doing it, and FP/SP. A memory watchpoint, and the fastest way to find who corrupted something. |
| `MODEL2_IRQMODE=0\|1\|2` | How the interrupt handler is entered. See **The interrupt frame** below. |

A game project normally adds its own field limit (Virtua Cop uses
`VCOP_MAX_FRAMES=N`), because the guest's busy-wait gives no other place to
stop. `model2recomp_set_frame_limit()` is the API behind it.

## The hang

> The window opens, nothing is drawn, and the process never exits.

`MODEL2_TRACE=200` and look at the **last line**. That is the function the
guest is spinning in. Then:

- **Spinning on `0x0098000C`?** Your bus is not routing that read to
  `model2recomp_field_sync()`. Nothing advances without it. See
  [execution-model.md](execution-model.md).
- **Spinning on `0x00980004`?** The output-FIFO-empty polarity is backwards,
  or the coprocessor never booted.
- **Spinning on the I/O board DPRAM?** A command was raised and never
  acknowledged.
- **Spinning somewhere in game code?** It is waiting on a value that a
  subsystem should have produced. Find what it loads in the loop, and work out
  who writes it.

## The black screen

> It runs, the frame counter advances, nothing is drawn.

Check in this order:

1. **Is the geometry engine getting a list?** `geo_polygon_count()` after a
   parse. Zero means either the game has not submitted one yet (attract mode
   can take 20 seconds of game time) or the display list is not reaching buffer
   RAM.
2. **Are polygons being rejected?** Culling, clipping and the z-sort each throw
   work away. A count that is non-zero before clipping and zero after is a
   clipping or viewport problem.
3. **Are they being drawn black?** That is almost always the palette, not the
   rasterizer — `palram[0x1000 + colorbase]` reading zero selects ramp 0 for
   every channel. See the colour path in
   [graphics-pipeline.md](graphics-pipeline.md#the-colour-path).

## `[func_table] MISS` lines

```
[func_table] MISS: no function at 0x00002564
```

Mostly harmless. `bx (gN)` returning through a saved `g14` looks like an
indirect branch to an address that is not a function entry; a miss returns to
the caller, which is the correct behaviour. The log is capped at 20 lines.

A miss on an address you *expected* to be a function is different — that is a
function your lifter never discovered. Common causes: it is only reachable
through an interrupt table, or it sits past alignment padding that the scan
treated as the end of the previous function.

## Finding out why guest code never runs

This is the technique that answers "why does this function never execute", and
it is mechanical rather than clever:

1. Build a **callee → caller** map from the generated C — every
   `func_table_call(0xADDR)` and every direct call gives you an edge.
2. Capture the set of addresses that appear in a `MODEL2_TRACE` run.
3. **Breadth-first search backwards** from the function you expected to run,
   until you hit one that did.

The first unexecuted function on that path is the gate — the thing that decides
not to call the next. Go read it.

If the search reports *no executed ancestor at all*, the function is not
reachable in your call graph, which usually means the lifter never found its
entry point. That is how Virtua Cop's scene renderer was found.

## The interrupt frame

This one is worth knowing about before it wastes your afternoon, because it
produces symptoms that look like anything but what it is.

Recompiled code has no instruction boundary to interrupt, so the handler is
called at the field boundary. On real hardware, taking an interrupt **pushes a
stack frame**, and the handler ends in a plain `ret` that pops it. Called bare,
that `ret` pops a frame nobody pushed — once per field.

The frame pointer then walks down the chain and, within a few hundred fields,
leaves work RAM entirely:

```
[watch] ... fp=00500500   [watch] ... fp=005004C0   [watch] ... fp=00500440
[watch] ... fp=00000000   [watch] ... fp=00000100   [watch] ... fp=0A0009C0
```

After that every frame-relative load in the guest reads whatever is at that
address — ROM, or nothing. A compiler stores short-lived temporaries relative
to the frame pointer, so the guest starts computing with values it never wrote.
On Virtua Cop one of them is a palette fade counter, and the resulting bogus
fade copies the i960's boot header over the polygon palette, which is why most
of its scenery renders black.

`MODEL2_IRQMODE` selects the model:

| Mode | What |
|---|---|
| `0` (default) | Call the handler bare. Wrong, but it is the mode in which the reference title renders. |
| `1` | `i960_do_call` first, so the handler's `ret` pops its own frame. What the hardware does. |
| `2` | Snapshot the whole context, call, restore. Equivalent guarantee, simpler. |

**Modes 1 and 2 stop the game submitting any display list at all** — permanently,
from the first field. It is not stuck: it executes *more* distinct functions
than mode 0 does, so the faithful model lets it get further into its own logic
and then somewhere else goes wrong. Whatever that is has not been found yet. If
you are looking for one thing to fix in this library, it is this.

## Comparing against MAME

MAME is the ground truth for this hardware, and running the same ROM set in it
side by side settles most arguments about what the picture should look like.
For anything numeric — a matrix, a display list, a palette — it is faster to
add a temporary `fprintf` here and reason about the values than to try to catch
the same moment in an emulator.

Two things worth knowing when you do:

- The attract sequence is **not deterministic** across runs here, because the
  field boundary is driven by wall-clock time. Two runs stopped at the same
  field number can be in different scenes. Sample with `MODEL2_SHOT_EVERY`
  rather than trusting a single frame number.
- Values that look like garbage often are not. `2.29589e-41` as a float is the
  bit pattern `0x00004000` — a small integer read as a float. Denormals in a
  dump usually mean you are looking at the wrong kind of data, not corrupt
  data.
