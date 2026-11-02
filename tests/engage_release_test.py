import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from make_engage_source_release import audit_member


class ReleaseBoundaryTest(unittest.TestCase):
    def test_private_outputs_and_links_are_rejected(self):
        for name in ('build/engage.exe', 'generated/main.c', 'config.local.toml', 'prod.keys',
                     'icon.jpg', 'engage.ico', 'Data/test.bundle', 'logs/engage.log',
                     'profile/account.uid', '../outside', '/absolute', 'tools/helper.exe'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                audit_member(name, b'private')
        with self.assertRaises(ValueError):
            audit_member('link', b'target', regular=False)

    def test_content_is_checked_even_under_a_safe_name(self):
        marker = 'TOTK_RECOMP_' + 'GENERATED_FROM_GAME_CODE'
        for content in (marker.encode(), ('title_key = ' + 'ab' * 16).encode(), b'NSO0' + bytes(32), b'MZ' + bytes(32)):
            with self.subTest(content=content[:8]), self.assertRaises(ValueError):
                audit_member('tools/innocent.py', content)
        audit_member('runtime/audout.c', b'// Original host runtime source.\n')
        audit_member('runtime/totk.ico', b'upstream source logo')


if __name__ == '__main__':
    unittest.main()
