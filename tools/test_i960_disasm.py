#!/usr/bin/env python3
"""Decode a handful of i960 encodings whose text is known.

Not a conformance suite - the real cross-check is against IDA, and it needs
IDA. This catches the kind of mistake that has actually happened here: an
opcode moved in the table, or an operand-mode bit read from the wrong place.
The src1 literal flag in particular was read from bit 5 for a long time, which
prints "subo sp, g4, g4" where the instruction is "subo 1, g4, g4".
"""

import struct

from i960_disasm import disasm_one


def reg(op, ext, src1, src2, dst, m1=0, m2=0):
    """Assemble a REG-format word. Mirrors the decode, deliberately."""
    return ((op << 24) | (ext << 7) | (dst << 19) | (src2 << 14) | src1
            | (m1 << 11) | (m2 << 12))


def ctrl(op, disp):
    return (op << 24) | (disp & 0x00FFFFFC)


G0, G1, G2 = 16, 17, 18

CASES = [
    (ctrl(0x08, 0x100), 'b 0x00000100'),
    (ctrl(0x09, 0x200), 'call 0x00000200'),
    (ctrl(0x0A, 0), 'ret'),

    (reg(0x59, 0x0, G0, G1, G2), 'addo g0, g1, g2'),
    (reg(0x58, 0x1, G0, G1, G2), 'and g0, g1, g2'),

    # Bit 11 makes src1 a 5-bit literal; bit 5 must not be mistaken for it.
    (reg(0x59, 0x2, 3, G1, G2, m1=1), 'subo 3, g1, g2'),
    (reg(0x59, 0x2, 1, G1, G2), 'subo sp, g1, g2'),

    # Bit 12 does the same for src2.
    (reg(0x59, 0x0, G0, 7, G2, m2=1), 'addo g0, 7, g2'),
]


def main():
    for word, want in CASES:
        text = disasm_one(struct.pack('<I', word), 0, 0)[0]
        assert text == want, f'0x{word:08X}: got {text!r}, want {want!r}'
    print(f'ok ({len(CASES)} encodings)')


if __name__ == '__main__':
    main()
