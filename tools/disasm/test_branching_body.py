"""An immediate code pointer whose ret is not on the straight line.

probes_as_returning_body walks one straight line to a ret, so it refused two
HP:CoS callbacks stored by immediate: 0x00038030, whose main path tail-jumps
and whose ret sits behind a `je`, and 0x00115BA0, which has no ret at all and
leaves by `jmp` into a known function. probes_as_branching_body follows the
branches; these pin both shapes and that data still fails it.
"""
import struct
from tools.disasm.test_decode_at import _engine, BASE


def engine_for(code):
    engine, section = _engine(BASE, code)
    engine.image.read_bytes_at_va = lambda addr, size: code[addr-BASE:addr-BASE+size]
    return engine


def rel32(at, target):
    return struct.pack('<i', target - (at + 5))


def test_ret_behind_a_conditional_branch():
    # mov eax,[esp+4]; test eax,eax; je +5; jmp far; ret
    code = bytes.fromhex('8b442404' '85c0' '7405') + b'\xe9' + rel32(BASE + 8, BASE + 0x4000) + b'\xc3'
    e = engine_for(code)
    assert not e.probes_as_returning_body(BASE)
    assert e.probes_as_branching_body(BASE)


def test_loop_before_the_ret():
    # xor ecx,ecx; inc ecx; cmp ecx,4; jl -5; ret  -- a backward jcc first
    code = bytes.fromhex('33c9' '41' '83f904' '7cfa' 'c3')
    assert engine_for(code).probes_as_branching_body(BASE)


def test_tail_jump_into_a_known_function_is_a_way_out():
    # push ecx; pop ecx; jmp BASE+0x4000 -- no ret anywhere
    code = bytes.fromhex('51' '59') + b'\xe9' + rel32(BASE + 2, BASE + 0x4000)
    e = engine_for(code)
    assert not e.probes_as_branching_body(BASE)
    assert e.probes_as_branching_body(BASE, known_entries={BASE + 0x4000})


def test_a_bad_byte_on_any_path_rejects():
    # je to an undefined opcode (0f 0b is ud2; 0xd6 is invalid), else ret
    code = bytes.fromhex('7401' 'c3' 'd6d6d6d6')
    assert not engine_for(code).probes_as_branching_body(BASE)


def test_int3_padding_is_not_code():
    code = bytes.fromhex('7401' 'c3' 'cccccccc')
    assert not engine_for(code).probes_as_branching_body(BASE)


def _detector(code):
    from tools.disasm.functions import FunctionDetector
    engine = engine_for(code)
    return FunctionDetector(engine, engine.image, None, None)


def test_entry_slot_needs_alignment_and_padding():
    # BASE is 16-aligned. 15 nops, then the slot at BASE+0x10.
    det = _detector(b'\x90' * 0x10 + bytes.fromhex('c3') + b'\x90' * 0x0F + b'\xc3' * 0x10)
    assert det._looks_like_an_entry_slot(BASE + 0x10)       # after nop, aligned
    assert det._looks_like_an_entry_slot(BASE + 0x20)       # after nop, aligned
    assert not det._looks_like_an_entry_slot(BASE + 0x11)   # after ret, unaligned
