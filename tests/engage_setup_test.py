"""Installer safety and recovery tests, using synthetic files only."""
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from engage_setup.model import Form, TITLE, installed_state
from engage_setup.pipeline import Pipeline, redact, remove_work, validate_destination, write_json
from installer.events import Reporter


class SetupTest(unittest.TestCase):
    def test_form_requires_firmware_and_accepts_optional_dlcs(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            file = root / 'own-input'
            file.touch()
            form = Form(base=str(file), update=str(file), keys=str(file), firmware=str(root),
                        dest=str(root / 'install'), lang='en-US')
            self.assertEqual(form.problems(), [])
            command = form.command('python.exe')
            self.assertEqual(command[:5], ['python.exe', '-X', 'utf8', '-m', 'engage_setup'])
            self.assertNotIn('--dlc1', command)
            form.dlc1 = str(root / 'missing')
            self.assertIn('ui.missing_dlc1', form.problems())
            form.firmware = ''
            self.assertIn('ui.need_firmware', form.problems())

    def test_reject_foreign_folder_and_input_overlap(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            setup = root / 'setup'
            setup.mkdir()
            dest = root / 'install'
            dest.mkdir()
            (dest / 'important.txt').write_text('keep')
            with self.assertRaisesRegex(ValueError, 'not empty'):
                validate_destination(dest, [], setup)
            with self.assertRaisesRegex(ValueError, 'separate'):
                validate_destination(dest, [dest / 'game.nsp'], setup)
            with self.assertRaisesRegex(ValueError, 'outside'):
                validate_destination(setup / 'install', [], setup)
            with self.assertRaisesRegex(ValueError, 'outside'):
                validate_destination(setup / 'install', [], setup / '..' / 'setup')
            self.assertEqual((dest / 'important.txt').read_text(), 'keep')

    def test_state_identity_and_safe_cleanup(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            write_json(root / 'engage-install.json', {'format': 1, 'title_id': 'other'})
            self.assertIsNone(installed_state(root))
            write_json(root / 'engage-install.json', {'format': 1, 'title_id': TITLE})
            self.assertIsNotNone(installed_state(root))
            work = root / '.setup-work'
            work.mkdir()
            profile = root / 'profile'
            profile.mkdir()
            with self.assertRaisesRegex(ValueError, 'outside'):
                remove_work(profile, work)
            self.assertTrue(profile.is_dir())
            generated = work / 'generated'
            generated.mkdir()
            remove_work(generated, work)
            self.assertFalse(generated.exists())

    def test_failed_step_is_not_checkpointed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            pipeline = Pipeline(Form(dest=str(root / 'game')), SimpleNamespace(release='test'), Reporter(out=io.StringIO()))
            pipeline.check = lambda: None
            pipeline.fingerprint = lambda: 'synthetic'
            pipeline.extract = lambda: (_ for _ in ()).throw(ValueError('synthetic failure'))
            with self.assertRaisesRegex(ValueError, 'synthetic failure'):
                pipeline.run()
            state = installed_state(pipeline.dest)
            self.assertEqual(state['done'], [])
            self.assertEqual(pipeline.step, 'extract')

    def test_no_raw_hex_in_tool_log(self):
        secret = 'ab' * 32
        self.assertNotIn(secret, redact('tool failed with ' + secret))
        self.assertEqual(redact('ordinary error'), 'ordinary error')

    def test_prepare_retry_keeps_existing_settings_and_profile(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            pipeline = Pipeline(Form(dest=str(root), lang='en-US'), SimpleNamespace(), Reporter(out=io.StringIO()))
            pipeline.state = {'format': 1, 'title_id': TITLE, 'done': []}
            pipeline.command = lambda *args, **kwargs: None
            pipeline.dest.mkdir(exist_ok=True)
            pipeline.build.mkdir(parents=True)
            for name in ('engage.exe', 'romfs-index.bin', 'SDL3.dll', 'totk_xlate.exe', 'totk_xlate.id'):
                (pipeline.build / name).write_bytes(b'synthetic host output')
            for directory in (pipeline.extracted / 'ExeFS', pipeline.extracted / 'packed-romfs', pipeline.work / 'fonts'):
                directory.mkdir(parents=True)
                (directory / 'synthetic').touch()
            settings = root / 'engage.ini'
            settings.write_text('volume = 37\n')
            (root / 'profile').mkdir()
            save = root / 'profile/keep-save'
            save.write_bytes(b'keep')
            pipeline.prepare()
            pipeline.prepare()
            self.assertEqual(settings.read_text(), 'volume = 37\n')
            self.assertEqual(save.read_bytes(), b'keep')
            self.assertTrue((root / 'engage.exe').exists())
            self.assertTrue((root / 'romfs-index.bin').exists())

    def test_cancelled_step_is_not_checkpointed(self):
        with tempfile.TemporaryDirectory() as temp:
            pipeline = Pipeline(Form(dest=str(Path(temp) / 'game')), SimpleNamespace(release='test'), Reporter(out=io.StringIO()))
            pipeline.check = lambda: None
            pipeline.fingerprint = lambda: 'synthetic'
            pipeline.extract = lambda: (_ for _ in ()).throw(KeyboardInterrupt())
            with self.assertRaises(KeyboardInterrupt):
                pipeline.run()
            self.assertEqual(installed_state(pipeline.dest)['done'], [])


if __name__ == '__main__':
    unittest.main()
