#!/usr/bin/env python3
"""
i960KB disassembler.

The Model 2's CPU, so this is board-level and shared by every game built on
model2recomp. The opcode tables are MAME's (src/devices/cpu/i960) - see NOTICE.

i960 instruction formats:
  CTRL: 8-bit opcode + 24-bit displacement
  COBR: 8-bit opcode + condition bits + displacement
  REG:  8-bit opcode + register operands
  MEM:  8-bit opcode + memory addressing modes
"""

import struct


# i960 instruction formats:
#   CTRL: 8-bit opcode + 24-bit displacement
#   COBR: 8-bit opcode + condition bits + displacement
#   REG:  8-bit opcode + register operands
#   MEM:  8-bit opcode + memory addressing modes

# Register names
REG_NAMES = {
    0: 'pfp', 1: 'sp', 2: 'rip',
    **{i: f'r{i}' for i in range(3, 16)},
    16: 'g0', 17: 'g1', 18: 'g2', 19: 'g3',
    20: 'g4', 21: 'g5', 22: 'g6', 23: 'g7',
    24: 'g8', 25: 'g9', 26: 'g10', 27: 'g11',
    28: 'g12', 29: 'g13', 30: 'g14', 31: 'fp',
}

# CTRL format opcodes (bits 31-24)
CTRL_OPS = {
    0x08: 'b',      0x09: 'call',    0x0A: 'ret',     0x0B: 'bal',
    0x10: 'bno',     0x11: 'bg',      0x12: 'be',      0x13: 'bge',
    0x14: 'bl',      0x15: 'bne',     0x16: 'ble',     0x17: 'bo',
    0x18: 'faultno', 0x19: 'faultg',  0x1A: 'faulte',  0x1B: 'faultge',
    0x1C: 'faultl',  0x1D: 'faultne', 0x1E: 'faultle', 0x1F: 'faulto',
}

# REG format opcodes (bits 31-24 + bits 11-7 for extended)
# REG format opcodes, keyed by (opcode, extension).
#
# Taken from MAME's cpu/i960/i960.cpp, which is what the lifter follows, and
# cross-checked against IDA's i960 module over this ROM: of the 66 encodings
# that actually appear, the two agree on every one.
#
# The earlier table here was written from the i960 manual's layout and named a
# dozen encodings wrongly - mulo read as addino, movl as scanbit, movr as
# cvtir, cmpr as movr - which made roms/disasm.txt actively misleading.
REG_OPS = {
    (0x58, 0x00): 'notbit', (0x58, 0x01): 'and', (0x58, 0x02): 'andnot',
    (0x58, 0x03): 'setbit', (0x58, 0x04): 'notand', (0x58, 0x06): 'xor',
    (0x58, 0x07): 'or', (0x58, 0x08): 'nor', (0x58, 0x09): 'xnor',
    (0x58, 0x0A): 'not', (0x58, 0x0B): 'ornot', (0x58, 0x0C): 'clrbit',
    (0x58, 0x0D): 'notor', (0x58, 0x0E): 'nand', (0x58, 0x0F): 'alterbit',
    (0x59, 0x00): 'addo', (0x59, 0x01): 'addi', (0x59, 0x02): 'subo',
    (0x59, 0x03): 'subi', (0x59, 0x08): 'shro', (0x59, 0x0A): 'shrdi',
    (0x59, 0x0B): 'shri', (0x59, 0x0C): 'shlo', (0x59, 0x0D): 'rotate',
    (0x59, 0x0E): 'shli',
    (0x5A, 0x00): 'cmpo', (0x5A, 0x01): 'cmpi', (0x5A, 0x02): 'concmpo',
    (0x5A, 0x03): 'concmpi', (0x5A, 0x04): 'cmpinco',
    (0x5A, 0x05): 'cmpinci', (0x5A, 0x06): 'cmpdeco',
    (0x5A, 0x07): 'cmpdeci', (0x5A, 0x0C): 'scanbyte',
    (0x5A, 0x0E): 'chkbit',
    (0x5B, 0x00): 'addc', (0x5B, 0x02): 'subc',
    (0x5C, 0x0C): 'mov',
    (0x5D, 0x0C): 'movl',
    (0x5E, 0x0C): 'movt',
    (0x5F, 0x0C): 'movq',
    (0x60, 0x00): 'synmov', (0x60, 0x02): 'synmovq',
    (0x64, 0x00): 'spanbit', (0x64, 0x01): 'scanbit', (0x64, 0x04): 'dmovt',
    (0x64, 0x05): 'modac',
    (0x65, 0x05): 'modpc',
    (0x66, 0x00): 'calls', (0x66, 0x0D): 'flushreg',
    (0x67, 0x00): 'emul', (0x67, 0x01): 'ediv', (0x67, 0x04): 'cvtir',
    (0x67, 0x05): 'cvtilr', (0x67, 0x06): 'scalerl', (0x67, 0x07): 'scaler',
    (0x68, 0x00): 'atanr', (0x68, 0x01): 'logepr', (0x68, 0x02): 'logr',
    (0x68, 0x03): 'remr', (0x68, 0x05): 'cmpr', (0x68, 0x08): 'sqrtr',
    (0x68, 0x09): 'expr', (0x68, 0x0A): 'logbnr', (0x68, 0x0B): 'roundr',
    (0x68, 0x0C): 'sinr', (0x68, 0x0D): 'cosr', (0x68, 0x0E): 'tanr',
    (0x69, 0x00): 'atanrl', (0x69, 0x02): 'logrl', (0x69, 0x05): 'cmprl',
    (0x69, 0x08): 'sqrtrl', (0x69, 0x09): 'exprl', (0x69, 0x0A): 'logbnrl',
    (0x69, 0x0B): 'roundrl', (0x69, 0x0C): 'sinrl', (0x69, 0x0D): 'cosrl',
    (0x69, 0x0E): 'tanrl',
    (0x6C, 0x00): 'cvtri', (0x6C, 0x01): 'cvtril', (0x6C, 0x02): 'cvtzri',
    (0x6C, 0x03): 'cvtzril', (0x6C, 0x09): 'movr',
    (0x6D, 0x09): 'movrl',
    (0x6E, 0x01): 'movre', (0x6E, 0x02): 'cpysre',
    (0x70, 0x01): 'mulo', (0x70, 0x08): 'remo', (0x70, 0x0B): 'divo',
    (0x74, 0x01): 'muli', (0x74, 0x08): 'remi', (0x74, 0x0B): 'divi',
    (0x78, 0x0B): 'divr', (0x78, 0x0C): 'mulr', (0x78, 0x0D): 'subr',
    (0x78, 0x0F): 'addr',
    (0x79, 0x0B): 'divrl', (0x79, 0x0C): 'mulrl', (0x79, 0x0D): 'subrl',
    (0x79, 0x0F): 'addrl',
}

# How many operands each REG instruction prints. Anything not listed takes the
# full src1, src2, dst.
ONE_SRC_OPS = {
    'not', 'mov', 'movl', 'movt', 'movq', 'movr', 'movrl', 'movre',
    'scanbit', 'spanbit', 'cvtir', 'cvtilr', 'cvtri', 'cvtril',
    'cvtzri', 'cvtzril', 'sqrtr', 'sqrtrl', 'sinr', 'sinrl',
    'cosr', 'cosrl', 'tanr', 'tanrl', 'expr', 'exprl',
    'logbnr', 'logbnrl', 'roundr', 'roundrl', 'classr', 'classrl',
    'testno', 'testg', 'teste', 'testge', 'testl', 'testne', 'testle',
    'testo',
}

COMPARE_OPS = {
    'cmpo', 'cmpi', 'concmpo', 'concmpi', 'cmpr', 'cmprl',
    'cmpor', 'cmporl', 'chkbit', 'scanbyte',
}

NO_OPERAND_OPS = {'flushreg', 'fmark', 'mark', 'syncf', 'faultno'}


# COBR format opcodes
COBR_OPS = {
    0x20: 'testno', 0x21: 'testg',  0x22: 'teste',  0x23: 'testge',
    0x24: 'testl',  0x25: 'testne', 0x26: 'testle', 0x27: 'testo',
    0x30: 'bbc',    0x31: 'cmpobg', 0x32: 'cmpobe', 0x33: 'cmpobge',
    0x34: 'cmpobl', 0x35: 'cmpobne',0x36: 'cmpoble',0x37: 'bbs',
    0x38: 'cmpibno',0x39: 'cmpibg', 0x3A: 'cmpibe', 0x3B: 'cmpibge',
    0x3C: 'cmpibl', 0x3D: 'cmpibne',0x3E: 'cmpible',0x3F: 'cmpibo',
}

# MEM format opcodes
MEM_OPS = {
    0x80: 'ldob',   0x82: 'stob',   0x84: 'bx',     0x85: 'balx',
    0x86: 'callx',  0x88: 'ldos',   0x8A: 'stos',
    0x8C: 'lda',
    0x90: 'ld',     0x92: 'st',     0x98: 'ldl',     0x9A: 'stl',
    0xA0: 'ldt',    0xA2: 'stt',
    0xB0: 'ldq',    0xB2: 'stq',
    0xC0: 'ldib',   0xC2: 'stib',   0xC8: 'ldis',   0xCA: 'stis',
}


def sign_extend(val, bits):
    """Sign-extend a value from 'bits' width."""
    if val & (1 << (bits - 1)):
        val -= 1 << bits
    return val


def disasm_one(data, offset, addr):
    """Disassemble one i960 instruction. Returns (text, size, is_call, is_branch, target)."""
    if offset + 4 > len(data):
        return ('???', 4, False, False, None)

    word = struct.unpack_from('<I', data, offset)[0]
    opcode = (word >> 24) & 0xFF

    # CTRL format (opcodes 0x08-0x1F)
    if 0x08 <= opcode <= 0x1F:
        mnem = CTRL_OPS.get(opcode, f'ctrl_{opcode:02X}')
        disp = sign_extend(word & 0x00FFFFFC, 24)
        target = addr + disp

        is_call = (opcode == 0x09)  # call
        is_branch = opcode in (0x08, 0x0B, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17)
        is_ret = (opcode == 0x0A)

        if opcode == 0x0A:  # ret
            return (f'{mnem}', 4, False, False, None)
        else:
            return (f'{mnem} 0x{target:08X}', 4, is_call, is_branch, target)

    # COBR format (opcodes 0x20-0x3F)
    if 0x20 <= opcode <= 0x3F:
        mnem = COBR_OPS.get(opcode, f'cobr_{opcode:02X}')
        src1 = (word >> 19) & 0x1F
        src2 = (word >> 14) & 0x1F
        disp = sign_extend(word & 0x1FFC, 13)
        target = addr + disp

        m1 = (word >> 13) & 1  # src1 is literal if set
        s2_flag = (word >> 12) & 1

        src1_str = f'{src1}' if m1 else REG_NAMES.get(src1, f'r{src1}')
        src2_str = REG_NAMES.get(src2, f'r{src2}')

        if 0x20 <= opcode <= 0x27:
            # test instructions - only src1
            return (f'{mnem} {src1_str}', 4, False, False, None)
        else:
            return (f'{mnem} {src1_str}, {src2_str}, 0x{target:08X}', 4, False, True, target)

    # REG format (opcodes 0x58-0x7F)
    if 0x58 <= opcode <= 0x7F:
        ext = (word >> 7) & 0xF
        src1 = word & 0x1F
        src2 = (word >> 14) & 0x1F
        dst = (word >> 19) & 0x1F
        # Operand modes. src1's literal flag is bit 11, not bit 5 - reading
        # the wrong bit prints "subo sp, g4, g4" where the instruction is
        # "subo 1, g4, g4", because r1 is named sp. The lifter had the same
        # bug once and it corrupted every REG instruction with a literal.
        m1 = (word >> 11) & 1   # src1 is a 5-bit literal if set
        m2 = (word >> 12) & 1   # src2 is a 5-bit literal if set
        m3 = (word >> 13) & 1   # destination is a floating-point register

        mnem = REG_OPS.get((opcode, ext), f'reg_{opcode:02X}_{ext:X}')

        src1_str = f'{src1}' if m1 else REG_NAMES.get(src1, f'r{src1}')
        src2_str = f'{src2}' if m2 else REG_NAMES.get(src2, f'r{src2}')
        dst_str = REG_NAMES.get(dst, f'r{dst}')

        # Operand count follows the instruction, not the format.
        if mnem in ONE_SRC_OPS:
            return (f'{mnem} {src1_str}, {dst_str}', 4, False, False, None)
        if mnem in COMPARE_OPS:
            return (f'{mnem} {src1_str}, {src2_str}', 4, False, False, None)
        if mnem in NO_OPERAND_OPS:
            return (mnem, 4, False, False, None)
        return (f'{mnem} {src1_str}, {src2_str}, {dst_str}', 4, False, False, None)

    # MEM format (opcodes 0x80-0xCF)
    if 0x80 <= opcode <= 0xCF:
        mnem = MEM_OPS.get(opcode, f'mem_{opcode:02X}')
        src_dst = (word >> 19) & 0x1F
        abase = (word >> 14) & 0x1F
        mode = (word >> 10) & 0xF

        reg_str = REG_NAMES.get(src_dst, f'r{src_dst}')
        abase_str = REG_NAMES.get(abase, f'r{abase}')

        inst_size = 4
        addr_str = ''

        if not (word & 0x1000):
            # MEMA format: 13-bit offset, optional abase
            mem_offset = word & 0x1FFF
            if word & 0x2000:
                addr_str = f'0x{mem_offset:X}({abase_str})'
            else:
                addr_str = f'0x{mem_offset:X}'
        else:
            # MEMB format
            index = word & 0x1F
            index_str = REG_NAMES.get(index, f'r{index}')
            scale = (word >> 7) & 0x7
            scale_val = 1 << scale if scale else 1

            if mode == 0x4:
                addr_str = f'({abase_str})'
            elif mode == 0x5:
                # IP-relative: disp32 + addr_of_next_instruction
                if offset + 8 <= len(data):
                    disp = struct.unpack_from('<I', data, offset + 4)[0]
                    target_addr = disp + (addr + 8)
                    inst_size = 8
                    addr_str = f'0x{target_addr:08X}'
                else:
                    addr_str = '???'
                    inst_size = 8
            elif mode == 0x7:
                # abase + index*scale (4-byte, no disp)
                addr_str = f'({abase_str})[{index_str}*{scale_val}]'
            elif mode == 0xC:
                # absolute disp32 (8-byte)
                if offset + 8 <= len(data):
                    disp = struct.unpack_from('<I', data, offset + 4)[0]
                    inst_size = 8
                    addr_str = f'0x{disp:08X}'
                else:
                    addr_str = '???'
                    inst_size = 8
            elif mode == 0xD:
                # disp32 + abase (8-byte)
                if offset + 8 <= len(data):
                    disp = struct.unpack_from('<I', data, offset + 4)[0]
                    inst_size = 8
                    addr_str = f'0x{disp:08X}({abase_str})'
                else:
                    addr_str = '???'
                    inst_size = 8
            elif mode == 0xE:
                # disp32 + index*scale (8-byte)
                if offset + 8 <= len(data):
                    disp = struct.unpack_from('<I', data, offset + 4)[0]
                    inst_size = 8
                    addr_str = f'0x{disp:08X}[{index_str}*{scale_val}]'
                else:
                    addr_str = '???'
                    inst_size = 8
            elif mode == 0xF:
                # disp32 + abase + index*scale (8-byte)
                if offset + 8 <= len(data):
                    disp = struct.unpack_from('<I', data, offset + 4)[0]
                    inst_size = 8
                    addr_str = f'0x{disp:08X}({abase_str})[{index_str}*{scale_val}]'
                else:
                    addr_str = '???'
                    inst_size = 8
            else:
                addr_str = f'mode{mode:X}({abase_str})'

        is_call = (opcode == 0x86)  # callx
        is_branch = (opcode == 0x84)  # bx

        if opcode in (0x82, 0x8A, 0x92, 0x9A, 0xA2, 0xB2, 0xC2, 0xCA):
            # Store: st src, addr
            return (f'{mnem} {reg_str}, {addr_str}', inst_size, is_call, is_branch, None)
        else:
            # Load/branch: ld addr, dst
            return (f'{mnem} {addr_str}, {reg_str}', inst_size, is_call, is_branch, None)

    return (f'.word 0x{word:08X}', 4, False, False, None)


def find_functions(data, base_addr=0, max_size=None):
    """
    Discover functions by following calls and branches.
    Uses recursive descent starting from known entry points.
    """
    if max_size is None:
        max_size = len(data)

    functions = {}       # addr -> {'name': str, 'size': int, 'calls': set, 'callers': set}
    visited = set()      # addresses already disassembled
    call_targets = set() # addresses that are call targets
    work_queue = []      # addresses to explore

    # i960 boot: first 12 words at address 0 are the Initial Boot Record (IBR)
    # Word 0-3: initial bus configuration
    # Word 8: first instruction pointer (IP)
    # Word 12: initial system address table pointer (PRCB)
    if len(data) >= 48:
        # Read the IBR
        ibr = struct.unpack_from('<12I', data, 0)
        print(f"\n=== i960 Initial Boot Record (IBR) ===")
        for i, val in enumerate(ibr):
            print(f"  IBR[{i:2d}] = 0x{val:08X}")

        # The first IP is typically at IBR[8] for i960KB
        # But Model 2 uses a different boot sequence
        # Let's look for the PRCB and SAT
        print()

    # Look for function prologues: common i960 patterns
    # Pattern 1: The first code after IBR
    # Pattern 2: Targets of CALL instructions
    # Pattern 3: Entries in jump tables

    # Start by scanning for common prologue patterns
    # i960 functions often start with:
    #   - Saving registers (stl, stq)
    #   - Setting up frame (lda sp, ...)
    #   - Or just direct code

    # First, find all CALL/BAL targets by linear scan
    print("=== Linear scan for CALL/BRANCH targets ===")
    offset = 0
    instruction_count = 0
    while offset < max_size and offset < len(data):
        text, size, is_call, is_branch, target = disasm_one(data, offset, base_addr + offset)
        instruction_count += 1

        if target is not None and (is_call or is_branch):
            if 0 <= target < len(data):
                if is_call:
                    call_targets.add(target)

        offset += size

    print(f"  Total instructions scanned: {instruction_count}")
    print(f"  Call targets found: {len(call_targets)}")

    # Find additional functions via prologue detection
    # Common i960KB prologue: starts right after previous ret or at aligned address
    prologue_addrs = set()
    offset = 0
    prev_was_ret = False
    while offset < max_size and offset < len(data):
        word = struct.unpack_from('<I', data, offset)[0]
        opcode = (word >> 24) & 0xFF

        if prev_was_ret and word != 0:
            prologue_addrs.add(base_addr + offset)

        prev_was_ret = (opcode == 0x0A)  # ret
        offset += 4

    # Combine all discovered function addresses
    all_funcs = sorted(call_targets | prologue_addrs)

    # Filter out addresses that point to zero/invalid data
    valid_funcs = []
    for addr in all_funcs:
        if addr < len(data) - 4:
            word = struct.unpack_from('<I', data, addr)[0]
            if word != 0 and word != 0xFFFFFFFF:
                valid_funcs.append(addr)

    print(f"  Post-ret prologues: {len(prologue_addrs)}")
    print(f"  Total unique functions: {len(valid_funcs)}")

    return valid_funcs


def disassemble_region(data, start, end, base_addr=0):
    """Disassemble a region and return lines."""
    lines = []
    offset = start
    while offset < end and offset < len(data):
        addr = base_addr + offset
        text, size, is_call, is_branch, target = disasm_one(data, offset, addr)
        lines.append(f'{addr:08X}:  {text}')
        offset += size
    return lines

