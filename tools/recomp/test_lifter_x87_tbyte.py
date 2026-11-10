"""fld/fstp with an 80-bit (tbyte) memory operand.

The x87 loads and stores its own extended format, ten bytes. fld read it as a
4-byte float and fstp dropped it to a comment. MSVC's asin and acos load pi/2
that way for an argument of exactly +-1, so the result came out 7.9e-19, and
Harry Potter and the Chamber of Secrets' climb and chest probes, which take a
heading from asin, failed whenever Harry pushed straight at the object.
"""
import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block


def _mem(size, disp=0x21336E):
    return Operand(type="mem", mem_base=None, mem_index=None, mem_scale=1,
                   mem_disp=disp, mem_size=size)


def _lift(mnemonic, op_str, operand):
    insn = Instruction(0, 6, mnemonic, op_str, "db2d6e332100")
    insn.operands = [operand]
    lifted, _ = lift_basic_block(
        Lifter(), BasicBlock(start=0, instructions=[insn]))
    return chr(10).join(lifted)


class X87TbyteTest(unittest.TestCase):
    def test_fld_tbyte_reads_ten_bytes(self):
        generated = _lift("fld", "xword ptr [0x21336e]", _mem(10))
        self.assertIn("recomp_ld80(", generated)
        self.assertNotIn("MEMF(", generated)

    def test_fstp_tbyte_stores_and_pops(self):
        generated = _lift("fstp", "xword ptr [0x21336e]", _mem(10))
        self.assertIn("recomp_st80(", generated)
        self.assertIn("fp_pop();", generated)

    def test_fst_float_unchanged(self):
        generated = _lift("fst", "dword ptr [0x21336e]", _mem(4))
        self.assertIn("MEMF(", generated)
        self.assertNotIn("fp_pop();", generated)


if __name__ == "__main__":
    unittest.main()
