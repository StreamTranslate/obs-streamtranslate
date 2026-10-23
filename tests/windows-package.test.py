"""Offline package checks; these do not execute OBS or the DLL on Windows."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('windows_package', ROOT / 'scripts/package-windows.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class WindowsPackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.dll = self.root / 'obs-streamtranslate.dll'
        self.ca = self.root / 'cacert.pem'
        self.dll.write_bytes(b'MZ-test-plugin-payload')
        self.ca.write_bytes(b'-----BEGIN CERTIFICATE-----\ntest\n-----END CERTIFICATE-----\n')
        self.output = self.root / 'output'

    def package(self):
        return module.package_windows(self.dll, self.ca, self.output)

    def test_modern_and_older_standard_install_have_identical_binary_and_ca(self):
        modern, _ = self.package()
        with ZipFile(modern) as archive:
            self.assertIsNone(archive.testzip())
            for prefix in ('obs-streamtranslate/', 'obs-streamtranslate/bin/64bit/'):
                self.assertEqual(archive.read(prefix + self.dll.name), self.dll.read_bytes())
                self.assertEqual(archive.read(prefix + self.ca.name), self.ca.read_bytes())
            self.assertIn('obs-streamtranslate/data/', archive.namelist())
            self.assertFalse(any(name.startswith('obs-plugins/') for name in archive.namelist()))

    def test_older_portable_package_retains_existing_layout(self):
        _, legacy = self.package()
        with ZipFile(legacy) as archive:
            self.assertEqual(set(archive.namelist()), {'obs-plugins/64bit/obs-streamtranslate.dll', 'obs-plugins/64bit/cacert.pem'})
            self.assertEqual(archive.read('obs-plugins/64bit/obs-streamtranslate.dll'), self.dll.read_bytes())
            self.assertIsNone(archive.testzip())

    def test_all_static_libuv_link_steps_include_synchronization(self):
        # Recent libuv uses WaitOnAddress/WakeByAddressSingle. Both the DLL and
        # native update harness must link the Windows SDK import library.
        workflow = (ROOT / '.github/workflows/build.yml').read_text()
        commands = workflow.split('cl /nologo')[1:]
        uv_commands = [command.split('      - name:')[0] for command in commands
                       if 'libuv.lib' in command.split('      - name:')[0]]
        self.assertEqual(len(uv_commands), 2)
        for command in uv_commands:
            self.assertIn('Synchronization.lib', command)

    def test_missing_input_leaves_no_partial_package(self):
        self.ca.unlink()
        with self.assertRaises(FileNotFoundError): self.package()
        self.assertFalse(self.output.exists())

    def test_non_windows_binary_rejected(self):
        self.dll.write_bytes(b'not a Windows binary')
        with self.assertRaises(ValueError): self.package()
        self.assertFalse(self.output.exists())

    def test_missing_certificate_bundle_rejected(self):
        self.ca.write_bytes(b'error page')
        with self.assertRaises(ValueError): self.package()
        self.assertFalse(self.output.exists())

if __name__ == '__main__':
    unittest.main()
