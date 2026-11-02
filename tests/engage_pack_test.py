import hashlib
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from pack_engage_romfs import pack
from romfs_index import build_index


class PackTest(unittest.TestCase):
    def test_round_trip_and_overwrite_rejection(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'source'
            (source / 'Data').mkdir(parents=True)
            originals = {'Data/empty': b'', 'Data/hello': b'hello', 'Data/other': b'world'}
            for name, value in originals.items():
                (source / name).write_bytes(value)
            report = pack(source, root / 'packed')
            image = (root / 'packed' / 'romfs.bin').read_bytes()
            self.assertEqual(hashlib.sha256(image).hexdigest(), report['romfs_sha256'])
            for entry in report['files']:
                self.assertEqual(image[entry['offset']:entry['offset'] + entry['size']], originals[entry['path']])
            self.assertTrue(build_index(report).startswith(b'TOTKRFS1'))
            with self.assertRaises(ValueError):
                pack(source, root / 'packed')
            with self.assertRaises(ValueError):
                pack(source, source / 'bad')


if __name__ == '__main__':
    unittest.main()
