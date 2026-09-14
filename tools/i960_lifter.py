#!/usr/bin/env python3
"""
i960 static recompiler (lifter).

Reads an i960KB program binary and generates C that performs the equivalent
operations against model2recomp's runtime, one C function per discovered guest
function, dispatched through the function table.

The i960 is the Model 2's CPU, so this is board-level and shared by every game
built on model2recomp. The <prefix> argument names the game: it becomes the
function prefix, the file names, and the include directory the generated C
reaches into for that game's functions.h. (i960_ops.h is board-level and
comes from model2recomp.)

Usage:
    python i960_lifter.py <program.bin> <output_dir> <prefix>

    python ext/model2recomp/tools/i960_lifter.py roms/program.bin src/recomp vcop
"""

import bisect
import glob
import os
import sys
import re
import struct
from pathlib import Path
from collections import defaultdict


# Matches an emitted label line, e.g. "L_000005E4: ;"
_LABEL_RE = re.compile(r'^\s*L_([0-9A-Fa-f]{8})\s*:')
# Matches a goto referencing a label, e.g. "goto L_000005E4"
_GOTO_RE = re.compile(r'goto L_([0-9A-Fa-f]{8})')


# Register name mapping for C code
C_REG = {
    0: 'I960_R(0)/*pfp*/', 1: 'I960_SP', 2: 'I960_R(2)/*rip*/',
    **{i: f'I960_R({i})' for i in range(3, 16)},
    16: 'I960_G(0)', 17: 'I960_G(1)', 18: 'I960_G(2)', 19: 'I960_G(3)',
    20: 'I960_G(4)', 21: 'I960_G(5)', 22: 'I960_G(6)', 23: 'I960_G(7)',
    24: 'I960_G(8)', 25: 'I960_G(9)', 26: 'I960_G(10)', 27: 'I960_G(11)',
    28: 'I960_G(12)', 29: 'I960_G(13)', 30: 'I960_G(14)', 31: 'I960_FP',
}


def sign_extend(val, bits):
    if val & (1 << (bits - 1)):
        val -= 1 << bits
    return val


def get_reg_c(r):
    return C_REG.get(r, f'I960_R({r})')


def comment_safe(s):
    """Neutralize comment markers so an expression can be embedded inside a
    /* ... */ comment without prematurely closing it (C has no nested comments).
    Register annotations like I960_R(0)/*pfp*/ would otherwise break TODO lines."""
    return s.replace('/*', '(*').replace('*/', '*)')


def get_src_c(reg, is_literal):
    """Get C expression for a source operand (register or literal)."""
    if is_literal:
        return f'{reg}'  # literal value
    return get_reg_c(reg)


class I960Lifter:
    def __init__(self, data, base_addr=0):
        self.data = data
        self.base_addr = base_addr
        self.functions = {}   # addr -> list of C lines
        self.call_targets = set()
        self.branch_targets = defaultdict(set)  # func_addr -> set of label addrs

    def read32(self, offset):
        if offset + 4 <= len(self.data):
            return struct.unpack_from('<I', self.data, offset)[0]
        return 0

    def lift_function(self, func_addr, end_addr=None):
        """Lift one function starting at func_addr to C code."""
        lines = []
        offset = func_addr
        max_addr = end_addr if end_addr else min(func_addr + 0x10000, len(self.data))

        # First pass: find branch targets within this function, and the
        # returns hiding behind an indirect branch.
        #
        # A bal-called leaf that needs g14 for something else saves it first:
        #   mov g14, g1 / mov 0, g14 / ... / bx (g1)
        # That last instruction is a return, not a branch. Lifting it as a
        # dispatch misses - the landing site is in the middle of the caller,
        # which is not a function - and the frame is never given back. Track
        # which register still holds the copy and read the bx accordingly.
        local_labels = set()
        holds_g14 = set()
        leaf_returns = set()
        scan_offset = func_addr
        while scan_offset < max_addr:
            word = self.read32(scan_offset)
            opcode = (word >> 24) & 0xFF
            inst_size = 4

            # Check for 8-byte instructions (MEMB format with displacement)
            if 0x80 <= opcode <= 0xCF and (word & 0x1000):
                mode = (word >> 10) & 0xF
                if mode in (0x5, 0xC, 0xD, 0xE, 0xF):  # modes with disp32
                    inst_size = 8

            # Find branch targets
            target = None
            if 0x08 <= opcode <= 0x1F:  # CTRL
                disp = sign_extend(word & 0x00FFFFFC, 24)
                target = scan_offset + disp
            elif 0x20 <= opcode <= 0x3F:  # COBR
                disp = sign_extend(word & 0x1FFC, 13)
                target = scan_offset + disp

            if target is not None and func_addr <= target < max_addr:
                local_labels.add(target)

            # "mov g14, gN" saves the bal return address; any later write
            # to gN throws the copy away.
            if (opcode == 0x5C and ((word >> 7) & 0xF) == 0x0C
                    and not ((word >> 11) & 1) and (word & 0x1F) == G14_REG):
                holds_g14.add((word >> 19) & 0x1F)
            elif 0x58 <= opcode <= 0x7F:
                holds_g14.discard((word >> 19) & 0x1F)
            elif opcode == 0x84:
                # bx: MEMB (bit 12) mode 0x4 is the bare "(abase)" form.
                if ((word & 0x1000) and ((word >> 10) & 0xF) == 0x4
                        and ((word >> 14) & 0x1F) in holds_g14):
                    leaf_returns.add(scan_offset)
            elif opcode in MEM_DEST_OPS:
                holds_g14.discard((word >> 19) & 0x1F)

            # Without explicit bounds, the function ends at the first ret.
            # With bounds, lift the whole range so multi-block functions (a
            # conditional branch that skips over an early ret) keep all their
            # labels; trailing data past the final ret becomes dead code.
            if opcode == 0x0A and end_addr is None:
                max_addr = scan_offset + 4
                break

            scan_offset += inst_size

        self._leaf_returns = leaf_returns

        # Second pass: generate C code
        while offset < max_addr:
            addr = offset
            word = self.read32(offset)
            opcode = (word >> 24) & 0xFF

            # Emit label if this is a branch target. The trailing ';' is an
            # empty statement so a label is valid even when the next line is a
            # comment or the closing brace of the function.
            if addr in local_labels:
                lines.append(f'L_{addr:08X}: ;')

            c_code, inst_size = self._lift_instruction(word, addr)
            if c_code:
                for line in c_code:
                    lines.append(f'    {line}')

            offset += inst_size

            # Without explicit bounds, stop at the first ret.
            if opcode == 0x0A and end_addr is None:
                break

        lines = self._fixup_dangling_gotos(lines)
        # Falling off the end of the generated C is not the same as returning.
        #
        # Function bounds are a guess - each one runs to the next discovered
        # entry - so a real function is sometimes cut in half, and a path
        # through the first half then reaches the closing brace without ever
        # executing a "ret". On hardware that path simply carries on into the
        # following instructions. Here it returns to the caller instead, and
        # the stack frame the function allocated is never given back: one
        # leaked frame per call, and the guest stack climbs until it overwrites
        # the PRCB and the game's own variables above it.
        #
        # So fall *through* - dispatch to the address the body stopped at,
        # which is the next function, exactly as the hardware would.
        if end_addr is not None and max_addr < len(self.data):
            if not self._ends_in_transfer(lines):
                lines.append(f'    /* falls through into 0x{max_addr:08X} */')
                lines.append(f'    func_table_call(0x{max_addr:08X});')
                lines.append(f'    return;')

        self.functions[func_addr] = lines
        return lines

    @staticmethod
    def _ends_in_transfer(lines):
        """True if the last thing the body does is leave the function."""
        for line in reversed(lines):
            text = line.strip()
            if not text or text.startswith('/*') or text.endswith(': ;'):
                continue
            return text == 'return;' or text.startswith('goto ')
        return False

        self.functions[func_addr] = lines
        return lines

    @staticmethod
    def _fixup_dangling_gotos(lines):
        """Rewrite any goto whose target label was never emitted.

        Out-of-range branches (and bytes misdecoded as code in data regions)
        produce gotos to addresses with no label in this function. Route them
        through the function table as a tail call so the output always compiles
        and inter-function transfers still dispatch correctly at runtime.
        """
        emitted = set()
        for ln in lines:
            m = _LABEL_RE.match(ln)
            if m:
                emitted.add(m.group(1).upper())

        def make_repl(is_bal):
            def repl(mm):
                tgt = mm.group(1).upper()
                if tgt in emitted:
                    return mm.group(0)
                if is_bal:
                    # bal is a leaf call: the callee returns via bx (g14) to the
                    # instruction after the bal, so fall through, don't tail-call.
                    #
                    # g14 is 0 everywhere except inside a bal sequence - the i960
                    # ABI requires it, and Sega's compiler leans on it, using
                    # "st g14, addr" as a store-zero. The leaf is supposed to put
                    # it back (mov g14,g1 / mov 0,g14 / ... / bx (g1)), but a leaf
                    # that returns straight through bx (g14) does not, and then
                    # every later store-zero writes a return address instead. One
                    # of those lands in the palette fade counter and corrupts the
                    # polygon palette. Restoring the invariant here covers both
                    # shapes of leaf.
                    return f'{{ func_table_call(0x{tgt}); I960_G(14) = 0; }}'
                # A branch that leaves the function it is in has nowhere to
                # land when the landing site is not a registered function -
                # which happens whenever a real function's blocks are not
                # contiguous, as a switch dispatched through "bx" makes them.
                # The dispatch misses, control unwinds to the caller, and the
                # frame this function allocated is never given back. Give it
                # back: the same guard a missed "call" already gets. Two of
                # these leaked on the gameplay path, where the stack has only
                # the 2KB between 0x00500C00 and the game's own variables -
                # and it ran into them, landing a local store on the attract
                # state machine.
                #
                # Registering the landing sites instead was measured and kills
                # the attract 3D: splitting a genuine function costs more than
                # a leak does.
                return (f'{{ if (!func_table_call(0x{tgt})) i960_do_ret();'
                        f' return; }}')
            return repl

        return [_GOTO_RE.sub(make_repl(ln.rstrip().endswith('/* bal */')), ln)
                for ln in lines]

    def _lift_instruction(self, word, addr):
        """Lift a single instruction to C code. Returns (list of C lines, inst_size)."""
        opcode = (word >> 24) & 0xFF
        lines = []
        ret_size = 4  # default, updated for 8-byte MEM instructions

        # ---- CTRL format ----
        if 0x08 <= opcode <= 0x1F:
            disp = sign_extend(word & 0x00FFFFFC, 24)
            target = (addr + disp) & 0xFFFFFFFF

            if opcode == 0x08:  # b (unconditional branch)
                lines.append(f'goto L_{target:08X}; /* b 0x{target:08X} */')
            elif opcode == 0x09:  # call
                self.call_targets.add(target)
                lines.append(f'i960_do_call(0x{target:08X}, 0x{addr+4:08X});')
                # A call to a function that was never lifted must not leave the
                # frame it just pushed behind: the callee's "ret" is what would
                # have popped it, and there is no callee.
                lines.append(f'if (!func_table_call(0x{target:08X})) i960_do_ret(); '
                             f'/* call 0x{target:08X} */')
            elif opcode == 0x0A:  # ret
                lines.append(f'i960_do_ret(); /* ret */')
                lines.append(f'return;')
            elif opcode == 0x0B:  # bal (branch and link)
                lines.append(f'I960_G(14) = 0x{addr+4:08X}; /* bal 0x{target:08X} */')
                lines.append(f'goto L_{target:08X}; /* bal */')
            elif opcode == 0x10:  # bno
                lines.append(f'/* bno 0x{target:08X} - branch if unordered (NaN) */')
            elif opcode == 0x11:  # bg
                lines.append(f'if (i960_test_cc(COND_G)) goto L_{target:08X}; /* bg */')
            elif opcode == 0x12:  # be
                lines.append(f'if (i960_test_cc(COND_E)) goto L_{target:08X}; /* be */')
            elif opcode == 0x13:  # bge
                lines.append(f'if (i960_test_cc(COND_GE)) goto L_{target:08X}; /* bge */')
            elif opcode == 0x14:  # bl
                lines.append(f'if (i960_test_cc(COND_L)) goto L_{target:08X}; /* bl */')
            elif opcode == 0x15:  # bne
                lines.append(f'if (i960_test_cc(COND_NE)) goto L_{target:08X}; /* bne */')
            elif opcode == 0x16:  # ble
                lines.append(f'if (i960_test_cc(COND_LE)) goto L_{target:08X}; /* ble */')
            elif opcode == 0x17:  # bo
                lines.append(f'/* bo 0x{target:08X} - branch if ordered */')
            else:
                lines.append(f'/* TODO: ctrl opcode 0x{opcode:02X} target 0x{target:08X} */')
            return lines, ret_size

        # ---- COBR format ----
        if 0x20 <= opcode <= 0x3F:
            src1_reg = (word >> 19) & 0x1F
            src2_reg = (word >> 14) & 0x1F
            m1 = (word >> 13) & 1
            disp = sign_extend(word & 0x1FFC, 13)
            target = (addr + disp) & 0xFFFFFFFF

            src1 = get_src_c(src1_reg, m1)
            src2 = get_reg_c(src2_reg)

            # The test family does not branch. It writes the condition code
            # into a register as 0 or 1, which the compiler then turns into a
            # mask - "subo 0, gN, gN" then "and" - to select a value without a
            # branch. Dropping them leaves the register holding whatever was
            # in it, and the mask selects garbage. Virtua Cop's palette loader
            # wraps a counter exactly this way and then indexes a table of
            # palette pointers with the result, so a missing "testl" fed an
            # IEEE float to a DMA that expects an address.
            #
            # i960.cpp: the destination is the src1 field, and the condition
            # mask is the low three bits of the opcode - except testno, which
            # is true when no condition bit is set at all.
            if opcode == 0x20:  # testno
                dst = get_reg_c(src1_reg)
                lines.append(f'{dst} = i960_test_cc(0x07) ? 0 : 1; /* testno */')
            elif 0x21 <= opcode <= 0x27:
                names = {0x21: ('testg',  'COND_G'),
                         0x22: ('teste',  'COND_E'),
                         0x23: ('testge', 'COND_GE'),
                         0x24: ('testl',  'COND_L'),
                         0x25: ('testne', 'COND_NE'),
                         0x26: ('testle', 'COND_LE'),
                         0x27: ('testo',  '0x07')}
                name, cond = names[opcode]
                dst = get_reg_c(src1_reg)
                lines.append(f'{dst} = i960_test_cc({cond}) ? 1 : 0; /* {name} */')
            elif opcode == 0x38:  # cmpibno - ordered never holds for integers
                lines.append(f'op_cmpi((int32_t){src1}, (int32_t){src2}); /* cmpibno */')
            elif opcode == 0x3F:  # cmpibo - always taken for integers
                lines.append(f'op_cmpi((int32_t){src1}, (int32_t){src2});')
                lines.append(f'goto L_{target:08X}; /* cmpibo */')
            elif opcode == 0x30:  # bbc (branch if bit clear)
                lines.append(f'if (!({src2} & (1u << ({src1} & 31)))) goto L_{target:08X}; /* bbc */')
            elif opcode == 0x37:  # bbs (branch if bit set)
                lines.append(f'if ({src2} & (1u << ({src1} & 31))) goto L_{target:08X}; /* bbs */')
            elif opcode == 0x31:  # cmpobg
                lines.append(f'op_cmpo({src1}, {src2});')
                lines.append(f'if (i960_test_cc(COND_G)) goto L_{target:08X}; /* cmpobg */')
            elif opcode == 0x32:  # cmpobe
                lines.append(f'op_cmpo({src1}, {src2});')
                lines.append(f'if (i960_test_cc(COND_E)) goto L_{target:08X}; /* cmpobe */')
            elif opcode == 0x33:  # cmpobge
                lines.append(f'op_cmpo({src1}, {src2});')
                lines.append(f'if (i960_test_cc(COND_GE)) goto L_{target:08X}; /* cmpobge */')
            elif opcode == 0x34:  # cmpobl
                lines.append(f'op_cmpo({src1}, {src2});')
                lines.append(f'if (i960_test_cc(COND_L)) goto L_{target:08X}; /* cmpobl */')
            elif opcode == 0x35:  # cmpobne
                lines.append(f'op_cmpo({src1}, {src2});')
                lines.append(f'if (i960_test_cc(COND_NE)) goto L_{target:08X}; /* cmpobne */')
            elif opcode == 0x36:  # cmpoble
                lines.append(f'op_cmpo({src1}, {src2});')
                lines.append(f'if (i960_test_cc(COND_LE)) goto L_{target:08X}; /* cmpoble */')
            elif opcode == 0x39:  # cmpibg
                lines.append(f'op_cmpi((int32_t){src1}, (int32_t){src2});')
                lines.append(f'if (i960_test_cc(COND_G)) goto L_{target:08X}; /* cmpibg */')
            elif opcode == 0x3A:  # cmpibe
                lines.append(f'op_cmpi((int32_t){src1}, (int32_t){src2});')
                lines.append(f'if (i960_test_cc(COND_E)) goto L_{target:08X}; /* cmpibe */')
            elif opcode == 0x3B:  # cmpibge
                lines.append(f'op_cmpi((int32_t){src1}, (int32_t){src2});')
                lines.append(f'if (i960_test_cc(COND_GE)) goto L_{target:08X}; /* cmpibge */')
            elif opcode == 0x3C:  # cmpibl
                lines.append(f'op_cmpi((int32_t){src1}, (int32_t){src2});')
                lines.append(f'if (i960_test_cc(COND_L)) goto L_{target:08X}; /* cmpibl */')
            elif opcode == 0x3D:  # cmpibne
                lines.append(f'op_cmpi((int32_t){src1}, (int32_t){src2});')
                lines.append(f'if (i960_test_cc(COND_NE)) goto L_{target:08X}; /* cmpibne */')
            elif opcode == 0x3E:  # cmpible
                lines.append(f'op_cmpi((int32_t){src1}, (int32_t){src2});')
                lines.append(f'if (i960_test_cc(COND_LE)) goto L_{target:08X}; /* cmpible */')
            else:
                lines.append(f'/* TODO: COBR opcode 0x{opcode:02X} */')
            return lines, ret_size

        # ---- REG format ----
        if 0x58 <= opcode <= 0x7F:
            ext = (word >> 7) & 0xF
            src1_reg = word & 0x1F
            src2_reg = (word >> 14) & 0x1F
            dst_reg = (word >> 19) & 0x1F
            # Operand mode flags: m1=bit11 (0x800), m2=bit12 (0x1000).
            # When set, the corresponding src field is a 5-bit literal rather
            # than a register. (Bit 5 is NOT m1 - that earlier decode silently
            # corrupted every REG op with a literal src1, e.g. "addo 4, ...".)
            m1 = (word >> 11) & 1
            m2 = (word >> 12) & 1
            m3 = (word >> 13) & 1

            src1 = get_src_c(src1_reg, m1)
            src2 = get_src_c(src2_reg, m2)
            dst = get_reg_c(dst_reg)

            # Real operands: a mode bit selects an fp register (literal form)
            # or an ordinary register holding IEEE bits. Resolved statically
            # here; mirrors get_1_rif / get_2_rif / set_rif in the MAME core.
            def f_src(reg, mode, long_real=False):
                if not mode:
                    if long_real:
                        return f'i960_u2d(g_i960.r[{reg & 0x1E}], g_i960.r[{(reg & 0x1E) + 1}])'
                    return f'i960_u2f(g_i960.r[{reg}])'
                if reg < 4:
                    return f'g_i960.fp[{reg}]'
                return '1.0' if reg == 0x16 else '0.0'

            def f_dst(expr, long_real=False):
                if m3:
                    return f'g_i960.fp[{dst_reg & 3}] = {expr};'
                if long_real:
                    pair = dst_reg & 0x1E
                    return f'i960_d2u({expr}, &g_i960.r[{pair}], &g_i960.r[{pair + 1}]);'
                return f'g_i960.r[{dst_reg}] = i960_f2u({expr});'

            key = (opcode, ext)

            # ---- 0x58: logical operations ----
            if key == (0x58, 0x00):    lines.append(f'{dst} = {src2} ^ (1u << ({src1} & 31)); /* notbit */')
            elif key == (0x58, 0x01):  lines.append(f'{dst} = {src1} & {src2}; /* and */')
            elif key == (0x58, 0x02):  lines.append(f'{dst} = {src2} & ~{src1}; /* andnot */')
            elif key == (0x58, 0x03):  lines.append(f'{dst} = {src2} | (1u << ({src1} & 31)); /* setbit */')
            elif key == (0x58, 0x04):  lines.append(f'{dst} = ~{src2} & {src1}; /* notand */')
            elif key == (0x58, 0x06):  lines.append(f'{dst} = {src1} ^ {src2}; /* xor */')
            elif key == (0x58, 0x07):  lines.append(f'{dst} = {src1} | {src2}; /* or */')
            elif key == (0x58, 0x08):  lines.append(f'{dst} = ~({src1} | {src2}); /* nor */')
            elif key == (0x58, 0x09):  lines.append(f'{dst} = ~({src1} ^ {src2}); /* xnor */')
            elif key == (0x58, 0x0A):  lines.append(f'{dst} = ~{src1}; /* not */')
            elif key == (0x58, 0x0B):  lines.append(f'{dst} = {src2} | ~{src1}; /* ornot */')
            elif key == (0x58, 0x0C):  lines.append(f'{dst} = {src2} & ~(1u << ({src1} & 31)); /* clrbit */')
            elif key == (0x58, 0x0D):  lines.append(f'{dst} = ~{src2} | {src1}; /* notor */')
            elif key == (0x58, 0x0E):  lines.append(f'{dst} = ~({src1} & {src2}); /* nand */')
            elif key == (0x58, 0x0F):  lines.append(f'{dst} = ({src2} & ~(1u << ({src1} & 31))) | (({dst} & 1) << ({src1} & 31)); /* alterbit */')

            # ---- 0x59: arithmetic + shifts ----
            elif key == (0x59, 0x00):  lines.append(f'{dst} = {src1} + {src2}; /* addo */')
            elif key == (0x59, 0x01):  lines.append(f'{dst} = {src1} + {src2}; /* addi */')
            elif key == (0x59, 0x02):  lines.append(f'{dst} = {src2} - {src1}; /* subo */')
            elif key == (0x59, 0x03):  lines.append(f'{dst} = {src2} - {src1}; /* subi */')
            elif key == (0x59, 0x08):  lines.append(f'{dst} = op_shro({src1}, {src2}); /* shro */')
            elif key == (0x59, 0x0A):  lines.append(f'{dst} = op_shrdi({src1}, {src2}); /* shrdi */')
            elif key == (0x59, 0x0B):  lines.append(f'{dst} = (uint32_t)((int32_t){src2} >> ({src1} & 31)); /* shri */')
            elif key == (0x59, 0x0C):  lines.append(f'{dst} = op_shlo({src1}, {src2}); /* shlo */')
            elif key == (0x59, 0x0D):  lines.append(f'{dst} = op_rotate({src1}, {src2}); /* rotate */')
            elif key == (0x59, 0x0E):  lines.append(f'{dst} = op_shlo({src1}, {src2}); /* shli */')

            # ---- 0x5A: compare ----
            elif key == (0x5A, 0x00):  lines.append(f'op_cmpo({src1}, {src2}); /* cmpo */')
            elif key == (0x5A, 0x01):  lines.append(f'op_cmpi((int32_t){src1}, (int32_t){src2}); /* cmpi */')
            elif key == (0x5A, 0x02):  lines.append(f'op_cmpo({src1}, {src2}); /* concmpo */')
            elif key == (0x5A, 0x03):  lines.append(f'op_cmpi((int32_t){src1}, (int32_t){src2}); /* concmpi */')
            elif key == (0x5A, 0x04):  lines.append(f'op_cmpo({src1}, {src2}); {dst} = {src2} + 1; /* cmpinco */')
            elif key == (0x5A, 0x05):  lines.append(f'op_cmpi((int32_t){src1}, (int32_t){src2}); {dst} = {src2} + 1; /* cmpinci */')
            elif key == (0x5A, 0x06):  lines.append(f'op_cmpo({src1}, {src2}); {dst} = {src2} - 1; /* cmpdeco */')
            elif key == (0x5A, 0x07):  lines.append(f'op_cmpi((int32_t){src1}, (int32_t){src2}); {dst} = {src2} - 1; /* cmpdeci */')
            elif key == (0x5A, 0x0E):  lines.append(f'op_chkbit({src1}, {src2}); /* chkbit */')

            # ---- 0x5C: mov ----
            elif key == (0x5C, 0x0C):  lines.append(f'{dst} = {src1}; /* mov */')

            # ---- 0x5D: movl (64-bit move, register pair) ----
            elif key == (0x5D, 0x0C):
                dst_pair = dst_reg & 0x1E
                src1_pair = src1_reg & 0x1E
                lines.append(f'g_i960.r[{dst_pair}] = g_i960.r[{src1_pair}]; g_i960.r[{dst_pair}+1] = g_i960.r[{src1_pair}+1]; /* movl */')

            # ---- 0x5E: movt (96-bit move, register triple) ----
            elif key == (0x5E, 0x0C):
                dst_triple = dst_reg & 0x1C
                src1_triple = src1_reg & 0x1C
                lines.append(f'g_i960.r[{dst_triple}] = g_i960.r[{src1_triple}]; g_i960.r[{dst_triple}+1] = g_i960.r[{src1_triple}+1]; g_i960.r[{dst_triple}+2] = g_i960.r[{src1_triple}+2]; /* movt */')

            # ---- 0x5F: movq (128-bit) / test ----
            elif key == (0x5F, 0x0C):
                dst_quad = dst_reg & 0x1C
                src1_quad = src1_reg & 0x1C
                lines.append(f'g_i960.r[{dst_quad}] = g_i960.r[{src1_quad}]; g_i960.r[{dst_quad}+1] = g_i960.r[{src1_quad}+1]; g_i960.r[{dst_quad}+2] = g_i960.r[{src1_quad}+2]; g_i960.r[{dst_quad}+3] = g_i960.r[{src1_quad}+3]; /* movq */')
            elif key == (0x5F, 0x00):  lines.append(f'{dst} = i960_test_cc(0x07) ? 0 : 1; /* testno */')
            elif key == (0x5F, 0x01):  lines.append(f'{dst} = op_testg(); /* testg */')
            elif key == (0x5F, 0x02):  lines.append(f'{dst} = op_teste(); /* teste */')
            elif key == (0x5F, 0x03):  lines.append(f'{dst} = op_testge(); /* testge */')
            elif key == (0x5F, 0x04):  lines.append(f'{dst} = op_testl(); /* testl */')
            elif key == (0x5F, 0x05):  lines.append(f'{dst} = op_testne(); /* testne */')
            elif key == (0x5F, 0x06):  lines.append(f'{dst} = op_testle(); /* testle */')
            elif key == (0x5F, 0x07):  lines.append(f'{dst} = 1; /* testo (always true) */')

            # ---- 0x60: synmov/synmovl/synmovq ----
            elif key == (0x60, 0x00):
                lines.append(f'bus_write32({src1}, bus_read32({src2})); /* synmov */')
            elif key == (0x60, 0x02):
                lines.append(f'bus_write32({src1}, bus_read32({src2})); bus_write32({src1}+4, bus_read32({src2}+4)); bus_write32({src1}+8, bus_read32({src2}+8)); bus_write32({src1}+12, bus_read32({src2}+12)); /* synmovq */')
                lines.append('/* A reinitialize IAC restarts the processor: nothing of this context survives, so unwind to the boot loop rather than run on. */')
                lines.append('if (bus_iac_reinit_pending()) return;')

            # ---- 0x64: spanbit, scanbit, modac ----
            elif key == (0x64, 0x00):
                lines.append(f'{{ uint32_t _v = {src1}; {dst} = 0xFFFFFFFF; for (int _i = 31; _i >= 0; _i--) {{ if (!(_v & (1u << _i))) {{ {dst} = _i; break; }} }} }} /* spanbit */')
            elif key == (0x64, 0x01):
                lines.append(f'{{ uint32_t _v = {src1}; {dst} = 0xFFFFFFFF; for (int _i = 31; _i >= 0; _i--) {{ if (_v & (1u << _i)) {{ {dst} = _i; break; }} }} }} /* scanbit */')
            elif key == (0x64, 0x05):
                lines.append(f'{dst} = g_i960.AC; g_i960.AC = (g_i960.AC & ~{src1}) | ({src2} & {src1}); /* modac */')

            # ---- 0x65: modpc ----
            elif key == (0x65, 0x05):
                lines.append(f'{dst} = g_i960.PC; g_i960.PC = (g_i960.PC & ~{src2}) | (g_i960.r[{dst_reg}] & {src2}); /* modpc */')

            # ---- 0x66: calls ----
            elif key == (0x66, 0x00):
                lines.append(f'/* calls {comment_safe(src1)} - system call */')

            # ---- 0x67: emul, ediv ----
            elif key == (0x67, 0x00):
                dst_pair = dst_reg & 0x1E
                lines.append(f'{{ uint64_t _r = (uint64_t){src1} * (uint64_t){src2}; g_i960.r[{dst_pair}+1] = (uint32_t)(_r >> 32); g_i960.r[{dst_pair}] = (uint32_t)_r; }} /* emul */')
            elif key == (0x67, 0x01):
                dst_pair = dst_reg & 0x1E
                src2_pair = src2_reg & 0x1E
                lines.append(f'{{ uint64_t _d = ((uint64_t)g_i960.r[{src2_pair + 1}] << 32) | g_i960.r[{src2_pair}]; if ({src1}) {{ g_i960.r[{dst_pair}] = (uint32_t)(_d % {src1}); g_i960.r[{dst_pair}+1] = (uint32_t)(_d / {src1}); }} }} /* ediv */')

            # ---- 0x674-0x677, 0x6C0-0x6C3: integer <-> real conversion ----
            elif key == (0x67, 0x04):
                lines.append(f_dst(f'(double)(int32_t){src1}') + ' /* cvtir */')
            elif key == (0x67, 0x05):
                lines.append(f_dst(f'(double)(int32_t){src1}', True) + ' /* cvtilr */')
            elif key == (0x67, 0x06):
                lines.append(f_dst(f'{f_src(src2_reg, m2, True)} * pow(2.0, (double)(int32_t){src1})', True) + ' /* scalerl */')
            elif key == (0x67, 0x07):
                lines.append(f_dst(f'{f_src(src2_reg, m2)} * pow(2.0, (double)(int32_t){src1})') + ' /* scaler */')

            elif key == (0x6C, 0x00):
                lines.append(f'{dst} = (uint32_t)(int32_t)i960_round({f_src(src1_reg, m1)}); /* cvtri */')
            elif key == (0x6C, 0x02):
                lines.append(f'{dst} = (uint32_t)(int32_t){f_src(src1_reg, m1)}; /* cvtzri */')
            elif key in ((0x6C, 0x01), (0x6C, 0x03)):
                pair = dst_reg & 0x1E
                conv = 'i960_round(' + f_src(src1_reg, m1) + ')' if ext == 0x01 else f_src(src1_reg, m1)
                mnem = 'cvtril' if ext == 0x01 else 'cvtzril'
                lines.append(f'{{ int64_t _v = (int64_t){conv}; g_i960.r[{pair}] = (uint32_t)_v; g_i960.r[{pair + 1}] = (uint32_t)((uint64_t)_v >> 32); }} /* {mnem} */')

            # ---- 0x680-0x68E / 0x690-0x69E: transcendental (real / long real) ----
            elif opcode in (0x68, 0x69):
                lr = (opcode == 0x69)
                a = f_src(src1_reg, m1, lr)
                b = f_src(src2_reg, m2, lr)
                unary = {0x08: f'sqrt({a})', 0x09: f'pow(2.0, {a}) - 1.0',
                         0x0A: f'logb({a})', 0x0B: f'i960_round({a})',
                         0x0C: f'sin({a})', 0x0D: f'cos({a})', 0x0E: f'tan({a})'}
                binary = {0x00: f'atan2({b}, {a})', 0x01: f'{b} * log2({a} + 1.0)',
                          0x02: f'{b} * log2({a})', 0x03: f'fmod({b}, {a})'}
                suffix = 'rl' if lr else 'r'
                if ext == 0x05:
                    lines.append(f'i960_cmp_d({a}, {b}); /* cmp{suffix} */')
                elif ext in unary:
                    lines.append(f_dst(unary[ext], lr) + f' /* op{ext:X}{suffix} */')
                elif ext in binary:
                    lines.append(f_dst(binary[ext], lr) + f' /* op{ext:X}{suffix} */')
                else:
                    lines.append(f'/* TODO: REG opcode=(0x{opcode:02X}, 0x{ext:X}) word=0x{word:08X} */')

            # ---- 0x6C9 movr / 0x6D9 movrl / 0x6E1 movre ----
            elif key == (0x6C, 0x09):
                lines.append(f_dst(f_src(src1_reg, m1)) + ' /* movr */')
            elif key == (0x6D, 0x09):
                lines.append(f_dst(f_src(src1_reg, m1, True), True) + ' /* movrl */')
            elif key == (0x6E, 0x01):
                lines.append(f_dst(f_src(src1_reg, m1, True), True) + ' /* movre (as long real) */')
            elif key == (0x6E, 0x02):
                # cpysre: src1's magnitude with src2's sign, long real.
                a = f_src(src1_reg, m1, True)
                b = f_src(src2_reg, m2, True)
                lines.append(f_dst(f'(({b}) >= 0.0 ? fabs({a}) : -fabs({a}))', True)
                             + ' /* cpysre */')

            elif key == (0x66, 0x0D):
                # flushreg writes the register cache out to the stack frames.
                # i960_do_call and i960_do_ret already keep that coherent, so
                # there is nothing to flush.
                lines.append(f'/* flushreg - register cache is already coherent */')

            # ---- 0x70: mulo, remo, divo ----
            elif key == (0x70, 0x01):  lines.append(f'{dst} = {src1} * {src2}; /* mulo */')
            elif key == (0x70, 0x08):  lines.append(f'{dst} = ({src1} != 0) ? ({src2} % {src1}) : 0; /* remo */')
            elif key == (0x70, 0x0B):  lines.append(f'{dst} = ({src1} != 0) ? ({src2} / {src1}) : 0; /* divo */')

            # ---- 0x74: muli, remi, divi ----
            elif key == (0x74, 0x01):  lines.append(f'{dst} = (uint32_t)((int32_t){src1} * (int32_t){src2}); /* muli */')
            elif key == (0x74, 0x08):  lines.append(f'{dst} = ({src1} != 0) ? (uint32_t)((int32_t){src2} % (int32_t){src1}) : 0; /* remi */')
            elif key == (0x74, 0x0B):  lines.append(f'{dst} = ({src1} != 0) ? (uint32_t)((int32_t){src2} / (int32_t){src1}) : 0; /* divi */')

            # ---- 0x78B-0x78F / 0x79B-0x79F: real arithmetic ----
            elif opcode in (0x78, 0x79) and ext in (0x0B, 0x0C, 0x0D, 0x0F):
                lr = (opcode == 0x79)
                a = f_src(src1_reg, m1, lr)
                b = f_src(src2_reg, m2, lr)
                op, mnem = {0x0B: ('/', 'div'), 0x0C: ('*', 'mul'),
                            0x0D: ('-', 'sub'), 0x0F: ('+', 'add')}[ext]
                lines.append(f_dst(f'{b} {op} {a}', lr) + f' /* {mnem}r{"l" if lr else ""} */')

            else:
                lines.append(f'/* TODO: REG opcode=(0x{opcode:02X}, 0x{ext:X}) word=0x{word:08X} */')
            return lines, ret_size

        # ---- MEM format ----
        if 0x80 <= opcode <= 0xCF:
            src_dst_reg = (word >> 19) & 0x1F
            abase_reg = (word >> 14) & 0x1F

            reg = get_reg_c(src_dst_reg)
            abase = get_reg_c(abase_reg)

            # Compute effective address using MAME's get_ea() logic
            ea = None
            inst_size = 4

            if not (word & 0x1000):
                # MEMA format (bit 12 = 0): 13-bit offset, optional abase
                offset_val = word & 0x1FFF
                if word & 0x2000:
                    # abase + offset
                    if offset_val == 0:
                        ea = f'{abase}'
                    else:
                        ea = f'({abase} + 0x{offset_val:X})'
                else:
                    # absolute offset (no abase)
                    ea = f'0x{offset_val:X}u'
            else:
                # MEMB format (bit 12 = 1): mode + index + scale
                mode = (word >> 10) & 0xF
                index_reg = word & 0x1F
                scale_bits = (word >> 7) & 0x7
                scale_val = 1 << scale_bits
                idx = get_reg_c(index_reg)

                if mode == 0x4:    # (abase) - register indirect
                    ea = f'{abase}'
                elif mode == 0x5:  # IP-relative: disp32 + (addr + 8)
                    disp = self.read32(addr + 4)
                    inst_size = 8
                    target = (disp + (addr + 8)) & 0xFFFFFFFF
                    ea = f'0x{target:08X}u /* IP-rel */'
                elif mode == 0x7:  # abase + index*scale (4-byte, no disp)
                    if scale_val == 1:
                        ea = f'({abase} + {idx})'
                    else:
                        ea = f'({abase} + ({idx} << {scale_bits}))'
                elif mode == 0xC:  # disp32 (absolute, 8-byte)
                    disp = self.read32(addr + 4)
                    inst_size = 8
                    ea = f'0x{disp:08X}u'
                elif mode == 0xD:  # disp32 + abase (8-byte)
                    disp = self.read32(addr + 4)
                    inst_size = 8
                    if disp == 0:
                        ea = f'{abase}'
                    else:
                        ea = f'({abase} + 0x{disp:08X}u)'
                elif mode == 0xE:  # disp32 + index*scale (8-byte)
                    disp = self.read32(addr + 4)
                    inst_size = 8
                    if scale_val == 1:
                        ea = f'(0x{disp:08X}u + {idx})'
                    else:
                        ea = f'(0x{disp:08X}u + ({idx} << {scale_bits}))'
                elif mode == 0xF:  # disp32 + abase + index*scale (8-byte)
                    disp = self.read32(addr + 4)
                    inst_size = 8
                    if scale_val == 1:
                        ea = f'(0x{disp:08X}u + {abase} + {idx})'
                    else:
                        ea = f'(0x{disp:08X}u + {abase} + ({idx} << {scale_bits}))'
                else:
                    ea = f'0 /* TODO: MEMB mode 0x{mode:X} */'

            # Generate load/store
            if opcode == 0x80:  # ldob
                lines.append(f'{reg} = op_ldob({ea}); /* ldob */')
            elif opcode == 0x82:  # stob
                lines.append(f'op_stob((uint8_t){reg}, {ea}); /* stob */')
            elif opcode == 0x84:  # bx (branch indirect)
                if ea == 'I960_G(14)' or addr in self._leaf_returns:
                    # bx (g14) is how a bal-called leaf procedure returns, and
                    # bx through a register still holding a saved copy of g14
                    # is the same return written the long way round. bal is
                    # lifted as a real call, so plain return is correct.
                    lines.append(f'return; /* bx {comment_safe(ea)} - leaf return */')
                else:
                    lines.append(f'/* bx {comment_safe(ea)} - indirect branch */')
                    lines.append(f'func_table_call({ea});')
                    lines.append(f'return;')
            elif opcode == 0x85:  # balx
                lines.append(f'{reg} = 0x{addr + inst_size:08X}; /* balx */')
                lines.append(f'func_table_call({ea});')
            elif opcode == 0x86:  # callx (indirect call)
                lines.append(f'i960_do_call({ea}, 0x{addr + inst_size:08X});')
                lines.append(f'if (!func_table_call({ea})) i960_do_ret(); /* callx */')
            elif opcode == 0x88:  # ldos
                lines.append(f'{reg} = op_ldos({ea}); /* ldos */')
            elif opcode == 0x8A:  # stos
                lines.append(f'op_stos((uint16_t){reg}, {ea}); /* stos */')
            elif opcode == 0x8C:  # lda (load address)
                lines.append(f'{reg} = {ea}; /* lda */')
            elif opcode == 0x90:  # ld
                lines.append(f'{reg} = op_ld({ea}); /* ld */')
            elif opcode == 0x92:  # st
                lines.append(f'op_st({reg}, {ea}); /* st */')
            elif opcode == 0x98:  # ldl
                lines.append(f'op_ldl({ea}, {src_dst_reg}); /* ldl */')
            elif opcode == 0x9A:  # stl
                lines.append(f'op_stl({src_dst_reg}, {ea}); /* stl */')
            elif opcode == 0xA0:  # ldt
                lines.append(f'op_ldt({ea}, {src_dst_reg}); /* ldt */')
            elif opcode == 0xA2:  # stt
                lines.append(f'op_stt({src_dst_reg}, {ea}); /* stt */')
            elif opcode == 0xB0:  # ldq
                lines.append(f'op_ldq({ea}, {src_dst_reg}); /* ldq */')
            elif opcode == 0xB2:  # stq
                lines.append(f'op_stq({src_dst_reg}, {ea}); /* stq */')
            elif opcode == 0xC0:  # ldib
                lines.append(f'{reg} = (uint32_t)op_ldib({ea}); /* ldib */')
            elif opcode == 0xC2:  # stib
                lines.append(f'op_stob((uint8_t){reg}, {ea}); /* stib */')
            elif opcode == 0xC8:  # ldis
                lines.append(f'{reg} = (uint32_t)op_ldis({ea}); /* ldis */')
            elif opcode == 0xCA:  # stis
                lines.append(f'op_stos((uint16_t){reg}, {ea}); /* stis */')
            else:
                lines.append(f'/* TODO: MEM opcode 0x{opcode:02X} ea={comment_safe(ea)} */')
            ret_size = inst_size
            return lines, ret_size

        # Unknown instruction
        lines.append(f'/* UNKNOWN: 0x{word:08X} at 0x{addr:08X} */')
        return lines, ret_size


# The original Model 2 maps program ROM 0x20000-0x3FFFF a second time at
# 0x00220000; 0x00200000-0x0021FFFF is RAM on that board. See bus.c.
# g14 in the register file: globals start at 16, so g14 is r30.
G14_REG = 30

# MEM opcodes that write their dst register, and so lose a saved copy of g14.
# Stores do not; the loads, lda and balx do.
MEM_DEST_OPS = frozenset({0x80, 0x85, 0x88, 0x8C, 0x90, 0x98,
                          0xA0, 0xB0, 0xC0, 0xC8})

ROM_ALIAS_LO = 0x00020000
ROM_ALIAS_HI = 0x00040000
ROM_ALIAS_BIAS = 0x00200000
ROM_ALIAS_WINDOW_LO = ROM_ALIAS_LO + ROM_ALIAS_BIAS
ROM_ALIAS_WINDOW_HI = ROM_ALIAS_HI + ROM_ALIAS_BIAS


def reinit_entries(data, max_size):
    """Entry points named by a reinitialize IAC message in the ROM.

    The boot stub hands control to the real firmware by sending itself an IAC
    "reinitialize processor" message: a 16-byte quad whose first word is
    0x93000000, third the new PRCB, fourth the new IP. No instruction branches
    to that IP, so scanning the code cannot find it - Daytona's stub sends the
    message and then branches to itself forever, waiting for it to take effect.

    The quad is a fixed i960 structure, so finding it is a scan rather than a
    guess. Virtua Cop has one of these too; its target was already reachable,
    which is why this went unnoticed.
    """
    entries = set()
    for addr in range(0, min(len(data), max_size) - 15, 4):
        if struct.unpack_from('<I', data, addr)[0] != 0x93000000:
            continue
        target = struct.unpack_from('<I', data, addr + 12)[0]
        if target & 3 or not (0 < target < max_size):
            continue
        entries.add(target)
    return entries


def interrupt_handlers(data, max_size):
    """Entry points reachable only through the i960 interrupt table.

    An interrupt handler is never called or branched to by any instruction, so
    scanning the code finds nothing. It is reached because the hardware reads
    its address out of the interrupt table, which the boot ROM builds. Walk
    there the way the processor does: SAT+4 gives the PRCB, PRCB+0x14 the
    interrupt table. The table's first nine words are pending state; words 9
    onward hold the handler for vectors 8, 9, 10...

    Without this, Virtua Cop's VBlank handler at 0x720 was never lifted as a
    function - it sat as unreachable trailing code inside its predecessor - and
    the game did no per-frame work at all.
    """
    def rd(addr):
        if addr < 0 or addr + 4 > len(data):
            return 0
        return struct.unpack_from('<I', data, addr)[0]

    prcb = rd(4)
    itab = rd(prcb + 0x14)
    if not itab or itab >= max_size:
        return set()

    handlers = set()
    for word in range(9, 0x400 // 4):
        target = rd(itab + word * 4)
        if 0 < target < max_size:
            handlers.add(target)
    return handlers


def discover_functions(data, max_size):
    """Find all function entry points."""
    from i960_disasm import disasm_one
    calls = set()
    post_ret = set()

    branch_targets = set()
    jump_targets = set()

    def harvest_jump_table(table_addr):
        """Read a switch dispatch table out of the ROM image.

        "ld table[gN*4], gM" followed by "bx (gM)" is how the compiler writes a
        switch. The entries are code addresses and nothing else in the program
        names them, so without this they are never discovered: the dispatch
        misses at runtime, the whole switch does nothing, and the frame the
        caller allocated is never given back.

        The table is unbounded here - the guard that limits it is a compare a
        few instructions earlier - so take entries while they still look like
        code addresses in this program and stop at the first that does not.
        """
        found = []
        addr = table_addr
        for _ in range(64):
            if addr + 4 > max_size:
                break
            entry = struct.unpack_from('<I', data, addr)[0]
            if entry & 3 or not (0x400 <= entry < max_size):
                break
            found.append(entry)
            addr += 4
        return found

    branch_edges = []      # (source address, target address) for plain branches

    pending_table = None   # (destination register, table address)

    offset = 0
    # Set once a ret is seen and cleared by the next real instruction, so that
    # alignment padding between functions is skipped rather than ending the
    # search. Virtua Cop's compiler pads with zero words, and the earlier
    # "the very next word after a ret" rule gave up on the first pad word -
    # which lost every function that is only ever reached through a function
    # pointer, since nothing in the code names its address either. The whole
    # scene renderer hung off one of them.
    after_ret = False
    while offset < max_size:
        text, size, is_call, is_branch, target = disasm_one(data, offset, offset)
        # A call into the ROM's second window names code this image already
        # holds, 0x200000 lower down. Daytona's runtime start is a list of
        # thirty calls, five of which are written that way.
        if target is not None and ROM_ALIAS_WINDOW_LO <= target < ROM_ALIAS_WINDOW_HI:
            target -= ROM_ALIAS_BIAS
        word = struct.unpack_from('<I', data, offset)[0]
        op = (word >> 24) & 0xFF
        # call (0x09) and bal (0x0B) both name a procedure entry; bal is the
        # leaf-call form used heavily by the Sega runtime library.
        is_entry = is_call or op == 0x0B
        if is_entry and target is not None and 0 < target < max_size:
            calls.add(target)

        # Plain branches name a label, never a procedure: CTRL b (0x08) and the
        # conditional forms (0x10-0x1F), and every COBR compare-and-branch.
        if target is not None and 0 < target < max_size:
            if op == 0x08 or 0x10 <= op <= 0x1F or 0x20 <= op <= 0x3F:
                branch_targets.add(target)
                branch_edges.append((offset, target))

        # "ld <32-bit displacement>[reg*4], dst" primes a possible switch;
        # a "bx (dst)" right after it confirms one.
        if op == 0x90 and size == 8 and (word & 0x1000) and ((word >> 10) & 0xF) >= 0xC:
            pending_table = ((word >> 19) & 0x1F,
                             struct.unpack_from('<I', data, offset + 4)[0])
        elif op == 0x84 and pending_table and ((word >> 14) & 0x1F) == pending_table[0]:
            jump_targets.update(harvest_jump_table(pending_table[1]))
            pending_table = None
        elif op != 0x90:
            pending_table = None

        is_padding = word == 0 or word == 0xFFFFFFFF
        if after_ret and not is_padding:
            post_ret.add(offset)
            after_ret = False
        if op == 0x0A:
            after_ret = True
        offset += size

    # A "ret" is not always the end of a function - a conditional branch that
    # skips over an early return leaves one in the middle. Code after such a
    # ret is a continuation, reached by that branch, and taking it for a
    # function entry cuts the real function in half: the second half gets its
    # own C function, and the path through it falls off the end without ever
    # reaching the ret that pops the frame. One leaked frame per field is
    # enough for the guest stack to climb into the PRCB inside a minute.
    #
    # So a post-ret candidate that something branches to is a label. One that
    # something *calls* is still a function, whichever else it is.
    demoted = post_ret & (branch_targets - calls)
    post_ret -= demoted

    candidates = (calls | post_ret | jump_targets |
                  interrupt_handlers(data, max_size) |
                  reinit_entries(data, max_size))

    def sift(cands):
        """Drop candidates that point at padding rather than code."""
        out = []
        for addr in sorted(cands):
            if addr < max_size - 4:
                word = struct.unpack_from('<I', data, addr)[0]
                if word != 0 and word != 0xFFFFFFFF:
                    out.append(addr)
        return out

    valid = sift(candidates)

    # ...but a branch that leaves the function it sits in is a tail call, not a
    # jump to a label. The lifter emits one as a dispatch through the function
    # table, so its target has to be registered or the transfer misses at
    # runtime and the frame is never given back. Daytona's firmware entry
    # tail-jumps to code after a ret in a function 1,700 bytes earlier, and
    # that address was demoted to a label it could never reach.
    #
    # Work out which function each end of the branch is in, and put back the
    # ones only a cross-function branch reaches.
    def owner(addr):
        i = bisect.bisect_right(valid, addr) - 1
        return valid[i] if i >= 0 else None

    # ponytail: one pass. Restoring an entry shifts the boundaries around it,
    # so a second pass could in principle restore more; nothing in either game
    # needs it, and a fixed point here is a loop over the whole program.
    restored = {t for src, t in branch_edges
                if t in demoted and owner(src) != owner(t)}
    if restored:
        valid = sift(candidates | restored)
    return valid


def main():
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} <program.bin> <output_dir> <prefix>")
        print("  prefix names the game: 'vcop' emits vcop_XXXXXXXX() into")
        print("  vcop_code_NNN.c and vcop_register_all(), and includes \"vcop/functions.h\".")
        sys.exit(1)

    prog_path = sys.argv[1]
    output_dir = sys.argv[2]
    prefix = sys.argv[3]
    guard = prefix.upper()
    os.makedirs(output_dir, exist_ok=True)
    # Stale output from a run that produced more files is still globbed by
    # CMake, and every function in it is defined twice.
    for stale in glob.glob(os.path.join(output_dir, f'{prefix}_code_*.c')):
        os.remove(stale)

    with open(prog_path, 'rb') as f:
        data = f.read()

    # Find program end
    prog_end = len(data)
    while prog_end > 0 and (data[prog_end - 1] in (0x00, 0xFF)):
        prog_end -= 1
    prog_end = (prog_end + 3) & ~3

    # Get entry point
    ip = struct.unpack_from('<I', data, 12)[0]
    prcb = struct.unpack_from('<I', data, 4)[0]
    fp = struct.unpack_from('<I', data, prcb + 24)[0]

    print(f'Program size: {prog_end} bytes')
    print(f'Entry point: 0x{ip:08X}')
    print(f'Initial FP: 0x{fp:08X}')

    # Discover functions
    print('Discovering functions...')
    func_addrs = discover_functions(data, prog_end)
    # Add entry point
    if ip not in func_addrs:
        func_addrs.append(ip)
    func_addrs.sort()
    print(f'Found {len(func_addrs)} functions')

    # Lift all functions
    lifter = I960Lifter(data)
    total_lines = 0

    # Determine function boundaries
    func_bounds = {}
    for i, addr in enumerate(func_addrs):
        end = func_addrs[i + 1] if i + 1 < len(func_addrs) else prog_end
        func_bounds[addr] = end

    print('Lifting functions to C...')
    for addr in func_addrs:
        end = func_bounds[addr]
        lines = lifter.lift_function(addr, end)
        total_lines += len(lines)

    print(f'Generated {total_lines} lines of C code')

    # Split into multiple output files (one per ~200 functions)
    FUNCS_PER_FILE = 200
    file_idx = 0
    funcs_in_file = 0
    current_file_lines = []
    all_func_addrs = sorted(lifter.functions.keys())

    # Write header with all declarations
    header_path = os.path.join(output_dir, f'{prefix}_funcs.h')
    with open(header_path, 'w') as f:
        f.write(f'/* Auto-generated - {prefix} recompiled function declarations */\n')
        f.write(f'#ifndef {guard}_FUNCS_H\n#define {guard}_FUNCS_H\n\n')
        f.write(f'#include "{prefix}/functions.h"\n\n')
        for addr in all_func_addrs:
            f.write(f'void {prefix}_{addr:08X}(void);\n')
        f.write('\n#endif\n')
    print(f'Wrote {header_path} ({len(all_func_addrs)} declarations)')

    # Write implementation files
    for addr in all_func_addrs:
        if funcs_in_file == 0:
            current_file_lines = []
            current_file_lines.append(f'/* Auto-generated - {prefix} recompiled i960 code */\n')
            current_file_lines.append('#include "model2recomp/i960_ops.h"\n')
            current_file_lines.append(f'#include "{prefix}_funcs.h"\n\n')

        lines = lifter.functions[addr]
        current_file_lines.append(f'/* Function at 0x{addr:08X} */\n')
        current_file_lines.append(f'void {prefix}_{addr:08X}(void)\n{{\n')
        for line in lines:
            current_file_lines.append(f'{line}\n')
        current_file_lines.append('}\n\n')
        funcs_in_file += 1

        if funcs_in_file >= FUNCS_PER_FILE or addr == all_func_addrs[-1]:
            file_path = os.path.join(output_dir, f'{prefix}_code_{file_idx:03d}.c')
            with open(file_path, 'w') as f:
                f.writelines(current_file_lines)
            print(f'Wrote {file_path} ({funcs_in_file} functions)')
            file_idx += 1
            funcs_in_file = 0

    # Write registration function
    reg_path = os.path.join(output_dir, f'{prefix}_register.c')
    with open(reg_path, 'w') as f:
        f.write(f'/* Auto-generated - {prefix} function registration */\n')
        f.write(f'#include "{prefix}/functions.h"\n')
        f.write(f'#include "{prefix}_funcs.h"\n\n')
        f.write(f'void {prefix}_register_all(void)\n{{\n')
        for addr in all_func_addrs:
            f.write(f'    func_table_register(0x{addr:08X}, {prefix}_{addr:08X});\n')
        # The original Model 2 shows program ROM 0x20000-0x3FFFF a second time
        # at 0x00220000, because 0x00200000-0x0021FFFF is RAM on that board.
        # A game whose program fills the whole 2MB region never notices; one
        # whose program is 256KB, like Daytona, is linked with its second half
        # at 0x227000 and calls itself there. Same code, two addresses, so
        # register both names for it.
        for addr in all_func_addrs:
            if ROM_ALIAS_LO <= addr < ROM_ALIAS_HI:
                f.write(f'    func_table_register(0x{addr + ROM_ALIAS_BIAS:08X}, '
                        f'{prefix}_{addr:08X});\n')
        f.write('}\n')
    print(f'Wrote {reg_path} ({len(all_func_addrs)} registrations)')

    print(f'\nDone! {len(all_func_addrs)} functions lifted to {file_idx} C files')


if __name__ == '__main__':
    main()
