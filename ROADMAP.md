# Roadmap

## Now

**Find which rendering stage is wrong.** The commands arriving from the guest
match MAME's almost exactly, and the output does not match MAME's picture, so
the fault is in this library. The useful next step is bisecting the pipeline
against MAME stage by stage — transform, culling, clipping, setup, fill — not
reasoning about the renderer as a whole. Virtua Cop's `tools/mame/` harness
produces the reference frames.

## Next

- **Sound.** A 68000 and a SCSP. Nothing is implemented; the sound interrupt is
  raised and then ignored.
- **The coprocessor data socket.** Virtua Cop leaves it empty, so the copro
  address decode currently returns 0 for anything with bit 23 set. Daytona USA
  populates it with 4MB, and that assumption will be wrong there.
- **Model 2A/2B/2C.** Only the original board is modelled. The later revisions
  change the coprocessor and the rasterizer; `docs/` has the survey of what
  differs.

## Deferred

- **Performance.** Roughly 22% of real speed with Virtua Cop. Correctness
  first — optimising a renderer that draws the wrong picture is wasted work.
- **Linux and macOS.** SDL throughout, nothing known to block it, nothing tried.

## Out of scope

- **Being an emulator.** This is a runtime for statically recompiled Model 2
  games. There is no CPU interpreter and no JIT, and there will not be one.
  MAME is already excellent at that job, and much of what is known here came
  from reading it.
- **Game-specific behaviour.** If a sentence is true of one game and not the
  board, it belongs in that game's repository.
- **Shipping ROM data or anything derived from it**, including test fixtures.
