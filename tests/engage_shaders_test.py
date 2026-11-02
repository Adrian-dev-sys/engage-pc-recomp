import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from engage_shader_census import records


class ShaderRecordsTest(unittest.TestCase):
    def test_record_bounds_and_control_magic(self):
        header = bytearray(b'BTCDBTCD' + bytes(0x30))
        control = bytes.fromhex('34127698') + bytes(0x87c)
        code = b'code'
        struct.pack_into('<II', header, 0x28, len(control), len(code))
        record = bytes(header) + code + control
        self.assertEqual(list(records(record)), [(control, code)])
        for damaged in (record[:-1], b'x' + record[1:], record[:0x3c] + bytes(len(control))):
            with self.subTest(size=len(damaged)), self.assertRaises(ValueError):
                list(records(damaged))


if __name__ == '__main__':
    unittest.main()
