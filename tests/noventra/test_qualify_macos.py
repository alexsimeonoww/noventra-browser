"""Exercise the CLI process boundary with owned synthetic programs, not a browser."""

import json
import platform
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/noventra'))
import qualify_macos as runtime


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='nv runtime # url-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def program(self, body):
        path = self.root / 'fixture'
        path.write_text(f'#!{sys.executable}\n' + body)
        path.chmod(0o700)
        return path

    def test_native_qualification_refuses_other_hosts_before_writing_output(self):
        output = self.root / 'native-result'
        for system, machine in [('Linux', 'arm64'), ('Darwin', 'x86_64')]:
            with self.subTest(system=system, machine=machine), \
                    patch.object(platform, 'system', return_value=system), \
                    patch.object(platform, 'machine', return_value=machine):
                with self.assertRaisesRegex(ValueError, 'Native Apple-silicon macOS'):
                    runtime.qualify(self.root / 'Noventra.app', output)
        self.assertFalse(output.exists())

    def test_private_launch_file_url_and_dom_markers(self):
        executable = self.program('''import pathlib,sys,urllib.parse
assert '--incognito' in sys.argv
assert '--use-mock-keychain' in sys.argv
assert '--no-sandbox' not in sys.argv
url = urllib.parse.urlsplit(sys.argv[-1])
assert url.scheme == 'file'
assert pathlib.Path(urllib.parse.unquote(url.path)).is_file()
print('<html data-noventra-smoke="passed"><p id="native-smoke">NOVENTRA_NATIVE_JS_42</p></html>')
''')
        result = runtime.run_probe(executable, self.root / 'out', 'dom', 3, '0.20.3.1')
        self.assertTrue(result['passed'])
        self.assertEqual(result['owned_processes_remaining'], [])
        self.assertIn('%23', result['argv'][-1])
        self.assertEqual(json.loads((self.root / 'out/result.json').read_text()), result)

    def test_zero_exit_without_marker_is_failure(self):
        executable = self.program('print("<!doctype html><p>PENDING</p>")\n')
        result = runtime.run_probe(executable, self.root / 'out', 'dom', 3, '0.20.3.1')
        self.assertEqual(result['exit'], 0)
        self.assertFalse(result['passed'])

    def test_nonzero_exit_cannot_pass_despite_version_marker(self):
        executable = self.program('import sys\nprint("Noventra 0.20.3.1 (Chromium 156.0.8078.79)")\nsys.exit(6)\n')
        result = runtime.run_probe(executable, self.root / 'out', 'version', 3, '0.20.3.1')
        self.assertTrue(result['version_marker'])
        self.assertFalse(result['passed'])

    def test_timeout_remains_failure_and_only_owned_child_is_terminated(self):
        executable = self.program('''import subprocess,sys,time
child=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)'])
print(child.pid,flush=True)
time.sleep(60)
''')
        unrelated = subprocess.Popen([sys.executable, '-c', 'import time;time.sleep(60)'])
        self.addCleanup(lambda: (unrelated.terminate(), unrelated.wait()))
        result = runtime.run_probe(executable, self.root / 'out', 'dom', .5, '0.20.3.1')
        self.assertTrue(result['timed_out'])
        self.assertFalse(result['passed'])
        self.assertEqual(result['owned_processes_remaining'], [])
        self.assertIsNone(unrelated.poll())
        pid = int((self.root / 'out/stdout.log').read_text().strip())
        table = runtime.process_table()
        self.assertTrue(pid not in table or 'time.sleep(60)' not in table[pid][1])


if __name__ == '__main__':
    unittest.main()
