# Execution Model

**Read this before you write a line of integration code.** Every other part of
this library behaves the way you would expect from its name. This one does not,
because a recompiled program is not a CPU and the difference shows up here.

## The problem

An emulator owns the schedule. It runs N cycles of CPU, then a scanline of
video, then some sound, and it can stop the CPU between any two instructions to
deliver an interrupt.

A statically recompiled program owns *itself*. `func_table_call(entry)` enters
the game's code and the C call stack is the game's call stack. There is no
outer loop to give time to anything else, and no instruction boundary to
interrupt.

Model 2 games make this worse in a specific, useful way: **they never return.**
Virtua Cop's `main` is an infinite loop that busy-waits on the video status
register for the next field. Call it and you never get control back.

## The solution: the frame boundary is inside the guest

The guest's wait loop reads bit 2 of `0x0098000C` over and over. That read is
routed to `model2recomp_field_sync()`, and *that* is the frame:

```
   guest code                    model2recomp
   ----------                    ------------
   ...
   build display list
   ...
   while (!(read(0x0098000C) & 4))   ---->  model2recomp_field_sync()
      ;                                       |
                                              |  has a field elapsed?
                                              |    no  -> return status unchanged
                                              |    yes -> geo_parse()            walk the list
                                              |           end_frame()            render + present
                                              |           trigger_vblank()       raise IRQ line 0
                                              |           dispatch_irq()   -----> calls the guest's
                                              |           begin_frame()            VBlank handler
                                              |           flip the field bit         (nested inside
                                              |                                       the wait loop)
   <------------------------------------------+
   loop exits, guest builds the next list
```

So one "frame" is: the guest asks whether the field has changed, and answering
that question is what makes it change.

`model2recomp_begin_frame()` / `model2recomp_end_frame()` exist and work, but
for a game that owns its loop they are called *by* `field_sync`, not by your
`main()`. Keep a loop over them in `main()` only as a fallback for the case
where the guest's init bails out early and does return.

### Why this matters for your bus

If your game project has its own bus layer, or you change `src/bus.c`, the one
routing you cannot get wrong is:

```c
/* 0x00980000 + 0x0C, read */
case 3: return model2recomp_field_sync();
```

Miss it and the guest spins forever on a status bit that never changes. That is
the single most common way a Model 2 recomp hangs, and
[`MODEL2_TRACE=N`](debugging.md) will name the function it is spinning in.

## Interrupts

There is nowhere to preempt, so interrupts are delivered at the field boundary
— which is where VBlank arrives on real hardware anyway, so for the interrupt
that matters this is not an approximation.

`model2recomp_dispatch_irq()` walks the hardware's own path rather than
hardcoding a handler:

1. **Interrupt controller** — `irq_request_read() & irq_enable_read()` gives
   the pending, enabled lines. The 12 controller bits drive four external i960
   lines, grouped `0x001`, `0x002`, `0x3FC`, `0xC00`.
2. **ICR** — the i960 interrupt control register maps each external line to a
   vector: `vector = (icr >> (line * 8)) & 0xFF`. A vector below 8 means the
   line is in IAC mode, which this hardware never uses.
3. **Interrupt table** — `PRCB + 0x14` points at it; the handler for a vector
   is at `table + 36 + (vector - 8) * 4`.
4. **Call it** — `func_table_call(handler)`.

Every pending line is serviced once per field, highest vector first (i960
priority is vector ÷ 8). Servicing only the first would starve every source but
VBlank, which is asserted every single field.

There is no nesting and no pre-emption: a handler runs to completion. A source
that must interrupt a running handler would need the interrupt table's
pending-priority words, which are not modelled.

### Your lifter must find the handlers

Interrupt handlers are never called or branched to by any instruction, so a
lifter that discovers functions by scanning for call targets will never find
them — they sit as unreachable trailing code inside whatever function precedes
them. Seed discovery from the interrupt table the way the processor reaches it:

```
SAT + 4  ->  PRCB  ->  PRCB + 0x14  ->  interrupt table  ->  handler per vector
```

## Boot

The i960 does not start at a fixed address. It reads an **initial memory
image** at `0x00000000`:

| Offset | Contents |
|---|---|
| `0x00` | SAT (system address table) pointer |
| `0x04` | PRCB (process control block) pointer |
| `0x0C` | First instruction pointer |
| `0x10` | Checksum |

The PRCB then supplies the initial stack:

```c
I960_FP = bus_read32(prcb + 0x18);   /* frame pointer */
I960_SP = I960_FP + 0x40;            /* one register-save area above it */
func_table_call(reset_ip);
```

Most Model 2 titles do **not** run their real firmware from that first IP. The
reset stub relocates the PRCB and interrupt table into work RAM, then issues an
IAC "reinitialize processor" message (type `0x93`) with `synmovq` to
`0xFF000010`, handing over a new PRCB and a new IP. `bus_iac_take_reinit()`
captures that, so the boot chain is *followed* rather than guessed:

```c
uint32_t ip = INITIAL_IP, prcb = INITIAL_PRCB;
for (int hop = 0; ip && hop < 8; hop++) {
    I960_FP = bus_read32(prcb + 0x18);
    I960_SP = I960_FP + 0x40;
    if (!func_table_call(ip)) break;      /* not registered - lifter missed it */
    ip = bus_iac_take_reinit(&prcb);      /* 0 when no reinit was issued */
}
```

For Virtua Cop this hops once: reset at `0x5D0`, then the real firmware entry
at `0x6A0`, which calls `main` and never comes back.

## Function dispatch

`func_table.h` is a hash table from i960 address to native function pointer:

```c
void        func_table_register(uint32_t i960_addr, i960_func_t func);
i960_func_t func_table_lookup(uint32_t i960_addr);
bool        func_table_call(uint32_t i960_addr);   /* false = no such function */
```

Your generated code calls `func_table_register` for every lifted function at
startup, and `func_table_call` wherever the original had an indirect branch or
call.

A **miss is not necessarily an error.** `bx (gN)` returning through a saved
`g14` looks like an indirect branch to an address that is not a function
entry; returning to the caller is the correct behaviour, and that is exactly
what a miss does. The miss log is capped at 20 lines for this reason. A miss on
an address you expected to be a real function *is* an error — usually a
function your lifter never discovered.

## Threading

There is none. The guest is single-threaded, the library is single-threaded,
and the coprocessor runs synchronously inside the bus access that needs its
result (see [tgp-coprocessor.md](tgp-coprocessor.md)). Nothing here is
thread-safe and nothing needs to be.
