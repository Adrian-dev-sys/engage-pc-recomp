import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from build_game import module_glue


class BuildIdentityTest(unittest.TestCase):
    def test_engage_has_its_own_identity(self):
        paths = {name: Path('build') / name for name in ('exefs', 'romfs', 'profile')}
        engage = module_glue([], [], {}, paths, 'engage-v200')
        totk = module_glue([], [], {}, paths, 'v143')
        self.assertIn('const int rt_game_id = 1;', engage)
        self.assertIn('const int rt_game_version = 200;', engage)
        self.assertIn('const int rt_game_id = 0;', totk)
        self.assertIn('const int rt_game_version = 143;', totk)


if __name__ == '__main__':
    unittest.main()
