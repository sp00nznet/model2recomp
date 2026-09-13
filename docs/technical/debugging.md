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
| `MODEL2_HOLD=test\|service\|start1\|coin1` | Hold a button down for a headless run. **Currently inert** — see [hardware-overview.md](hardware-overview.md#what-is-not-modelled); host input does not reach the guest yet. |

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
