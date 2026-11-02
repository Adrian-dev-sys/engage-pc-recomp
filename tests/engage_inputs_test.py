"""Synthetic headers test input rejection without any game data."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from engage_inputs import BUILDS, DLC_SPECS, inspect_dlcs, inspect_inputs
from check_content import problems_for


class EngageInputsTest(unittest.TestCase):
    def test_both_dlc_markers_are_required(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaisesRegex(ValueError, 'DLC 1'):
                inspect_dlcs(root)
            for index, marker in DLC_SPECS.values():
                folder = root / str(index)
                folder.mkdir()
                (folder / marker).touch()
            self.assertEqual(len(inspect_dlcs(root)), 2)
            (root / '2' / 'h0').write_bytes(b'bad')
            with self.assertRaisesRegex(ValueError, 'DLC 2'):
                inspect_dlcs(root)

    def test_content_boundary(self):
        for name in ('scene.bundle', 'data.unity3d', 'shared.assets', 'shared.resS',
                     'audio.wem', 'audio.bnk', 'global-metadata.dat',
                     'unity default resources', 'unity_builtin_extra', 'control.nacp', 'icon.jpg', 'engage.ico'):
            with self.subTest(name=name):
                self.assertTrue(problems_for(Path(name)))
        self.assertEqual(problems_for(Path('engage_inputs.py')), [])

    def test_inputs_and_rejections(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name, build in BUILDS.items():
                header = bytearray(0x100)
                header[:4] = b'NSO0'
                header[0x40:0x40 + len(bytes.fromhex(build))] = bytes.fromhex(build)
                (root / name).write_bytes(header)
            report = inspect_inputs(root)
            self.assertEqual(set(report['exefs_sha256']), set(BUILDS))
            self.assertFalse(report['romfs_marker_present'])
            with self.assertRaisesRegex(ValueError, 'RomFS'):
                inspect_inputs(root, root)
            (root / 'Data').mkdir()
            (root / 'Data' / 'data.unity3d').touch()
            self.assertTrue(inspect_inputs(root, root)['romfs_marker_present'])
            header[0x40] ^= 0xFF
            (root / 'sdk').write_bytes(header)
            with self.assertRaisesRegex(ValueError, 'build ID'):
                inspect_inputs(root)
            (root / 'sdk').write_bytes(b'NSO0')
            with self.assertRaisesRegex(ValueError, 'complete header'):
                inspect_inputs(root)
            (root / 'sdk').unlink()
            with self.assertRaises(FileNotFoundError):
                inspect_inputs(root)


if __name__ == '__main__':
    unittest.main()
