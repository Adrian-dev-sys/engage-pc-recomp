import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import extract_engage


class ExtractEngageTest(unittest.TestCase):
    def test_package_identity(self):
        valid = 'content Patch 0100a6301214e800 327680\n'
        extract_engage.check_content(valid, 'Patch', extract_engage.UPDATE, 327680)
        for output in ('', valid * 2, valid.replace('327680', '0'),
                       valid.replace('1214e800', '1214e000')):
            with self.subTest(output=output), self.assertRaises(ValueError):
                extract_engage.check_content(output, 'Patch', extract_engage.UPDATE, 327680)

    def test_tool_logs_redact_key_sized_values_and_fail_closed(self):
        secret = 'ab' * 16
        with tempfile.TemporaryDirectory() as temporary:
            logs = Path(temporary)
            for code in (0, 1):
                result = subprocess.CompletedProcess([], code, f'key = {secret}\n', '')
                with patch.object(extract_engage, 'LOGS', logs), patch.object(
                    extract_engage.subprocess, 'run', return_value=result
                ):
                    if code:
                        with self.assertRaises(ValueError):
                            extract_engage.run_tool(Path('prod.keys'), [], 'test')
                    else:
                        self.assertNotIn(secret, extract_engage.run_tool(Path('prod.keys'), [], 'test'))
                self.assertNotIn(secret, (logs / 'test.log').read_text())


if __name__ == '__main__':
    unittest.main()
