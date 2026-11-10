"""`in` and `out` lift to the runtime's port I/O, not to RECOMP_UNIMPL.

They used to be unimplemented, so `in al, dx` left al holding whatever it held
before. D3D8's vblank ISR and DPC read the video encoder's field signal that
way (port 0x80C0, bit 5), and HP:CoS's XMV player, waiting for the other
field, waited forever. These pin the accumulator width and the port operand
for both directions, and that an operand shape the lifter does not know still
carries the marker rather than lifting to something wrong.
"""
import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter


def _lift(mnemonic, op_str, operands):
    lifter = Lifter()
    insn = Instruction(0x001390FD, 1, mnemonic, op_str, "",
                       operands=list(operands))
    return lifter, " ".join(lifter.lift_instruction(insn))


def _reg(name):
    return Operand(type="reg", reg=name)


def _imm(value):
    return Operand(type="imm", imm=value)


class PortIoTest(unittest.TestCase):

    def test_in_al_dx_sets_only_the_low_byte(self):
        lifter, out = _lift("in", "al, dx", [_reg("al"), _reg("dx")])
        self.assertIn("SET_LO8(eax, xbox_PortIn(LO16(edx), 1));", out)
        self.assertNotIn("RECOMP_UNIMPL", out)
        self.assertEqual(lifter.unimplemented, {})

    def test_in_ax_sets_the_low_word(self):
        _, out = _lift("in", "ax, dx", [_reg("ax"), _reg("dx")])
        self.assertIn("SET_LO16(eax, xbox_PortIn(LO16(edx), 2));", out)

    def test_in_eax_from_an_immediate_port(self):
        _, out = _lift("in", "eax, 0xc", [_reg("eax"), _imm(0xC)])
        self.assertIn("eax = xbox_PortIn(0xCu, 4);", out)

    def test_out_dx_al_passes_width_one(self):
        _, out = _lift("out", "dx, al", [_reg("dx"), _reg("al")])
        self.assertIn("xbox_PortOut(LO16(edx), eax, 1);", out)

    def test_out_to_an_immediate_port(self):
        _, out = _lift("out", "0x68, eax", [_imm(0x68), _reg("eax")])
        self.assertIn("xbox_PortOut(0x68u, eax, 4);", out)

    def test_unknown_operand_shape_keeps_the_marker(self):
        # Not an accumulator: the encoding does not exist, and this is what a
        # decoder reading data would produce. It must not lift silently.
        lifter, out = _lift("in", "bl, dx", [_reg("bl"), _reg("dx")])
        self.assertIn("RECOMP_UNIMPL", out)
        self.assertIn("in", lifter.unimplemented)


if __name__ == "__main__":
    unittest.main()
