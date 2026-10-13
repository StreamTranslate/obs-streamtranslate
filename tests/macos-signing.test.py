"""Offline regression tests: rejected notarization must not publish a package."""
import json, os, pathlib, subprocess, tempfile, unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
class SigningTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=ROOT / 'tests')
        self.addCleanup(self.tmp.cleanup)
        self.path = pathlib.Path(self.tmp.name)
        self.bin = self.path / 'bin'; self.bin.mkdir()
        self.pkg = self.path / 'plugin.pkg'; self.pkg.write_text('unsigned')
        self.env = dict(os.environ, PATH=str(self.bin) + ':' + os.environ['PATH'], MACOS_INSTALLER_IDENTITY='Developer ID Installer: Test (TESTTEAM)', MACOS_TEAM_ID='TESTTEAM', NOTARY_KEYCHAIN_PROFILE='test-profile', NOTARY_STATUS='Accepted', CALL_LOG=str(self.path / 'calls'))
        stub = '''#!/usr/bin/env python3
import json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
with open(os.environ['CALL_LOG'], 'a') as f: f.write(name + ' ' + ' '.join(sys.argv[1:]) + '\\n')
if name == 'productsign': pathlib.Path(sys.argv[-1]).write_text('signed')
if name == 'pkgutil': print(os.environ['MACOS_INSTALLER_IDENTITY'])
if name == 'xcrun' and sys.argv[1:3] == ['notarytool', 'submit']:
    print(json.dumps({'id':'test-submission', 'status':os.environ['NOTARY_STATUS']}))
'''
        for name in ['productsign', 'pkgutil', 'xcrun', 'spctl']:
            file = self.bin / name; file.write_text(stub); file.chmod(0o755)
    def run_script(self):
        return subprocess.run(['bash', str(ROOT / 'scripts/macos-notarize.sh'), str(self.pkg)], env=self.env, capture_output=True, text=True)
    def test_accepted_staples_validates_and_promotes(self):
        result = self.run_script(); self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.pkg.read_text(), 'signed')
        calls = (self.path / 'calls').read_text()
        self.assertIn('stapler staple', calls); self.assertIn('stapler validate', calls); self.assertIn('spctl --assess --type install', calls)
    def test_rejected_keeps_original_and_never_staples(self):
        self.env['NOTARY_STATUS'] = 'Invalid'; result = self.run_script()
        self.assertNotEqual(result.returncode, 0); self.assertEqual(self.pkg.read_text(), 'unsigned')
        self.assertNotIn('stapler', (self.path / 'calls').read_text())
    def test_in_progress_is_not_success(self):
        self.env['NOTARY_STATUS'] = 'In Progress'; self.assertNotEqual(self.run_script().returncode, 0)
        self.assertEqual(self.pkg.read_text(), 'unsigned')
    def test_wrong_certificate_type_rejected_before_tools(self):
        self.env['MACOS_INSTALLER_IDENTITY'] = 'Apple Distribution: Test'
        self.assertNotEqual(self.run_script().returncode, 0); self.assertFalse((self.path / 'calls').exists())
    def test_missing_notary_profile_rejected_before_tools(self):
        self.env.pop('NOTARY_KEYCHAIN_PROFILE'); self.assertNotEqual(self.run_script().returncode, 0)
        self.assertFalse((self.path / 'calls').exists())
if __name__ == '__main__': unittest.main()
