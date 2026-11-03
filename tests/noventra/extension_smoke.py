"""Run a local MV3 fixture; does not certify Chrome Web Store compatibility."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import urllib.parse

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/noventra'))
from benchmark import request, stop_browser


def wait_for(check, process, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError('Browser exited; inspect browser.log')
        value = check()
        if value:
            return value
        time.sleep(.2)
    raise TimeoutError('Extension check timed out; inspect browser.log')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    browser = args.browser.resolve(strict=True)
    fixture = ROOT / 'tests/fixtures/extension-smoke'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    log_path = args.output.parent / 'browser.log'
    result = {'browser': subprocess.check_output([str(browser), '--version'], text=True).strip(),
              'mode': 'headless', 'passed': False,
              'scope': 'Local unpacked MV3 extension document, storage, worker messaging and tabs API; not toolbar clicking or store installation'}
    try:
        with tempfile.TemporaryDirectory(dir=args.output.parent, prefix='profile-') as temp:
            profile = Path(temp).resolve()
            with log_path.open('w') as log:
                process = subprocess.Popen([
                    str(browser), f'--user-data-dir={profile}', '--headless=new',
                    '--no-first-run', '--no-default-browser-check',
                    '--remote-debugging-address=127.0.0.1', '--remote-debugging-port=0',
                    f'--disable-extensions-except={fixture}', f'--load-extension={fixture}',
                    'about:blank'], stdout=log, stderr=log, start_new_session=True)
                try:
                    port_file = profile / 'DevToolsActivePort'
                    wait_for(port_file.exists, process)
                    port = int(port_file.read_text().splitlines()[0])
                    workers = wait_for(lambda: [t for t in request(port, '/json/list')
                                               if t['url'].startswith('chrome-extension://')
                                               and t['url'].endswith('/worker.js')], process)
                    origin = workers[0]['url'].rsplit('/', 1)[0]
                    # Allow the extension to become idle before opening its UI.
                    time.sleep(35)
                    url = urllib.parse.quote(origin + '/popup.html', safe='')
                    target = request(port, '/json/new?' + url, 'PUT')
                    pages = wait_for(lambda: [t for t in request(port, '/json/list')
                                              if t['id'] == target['id'] and
                                              (t['title'] == 'Extension checks passed' or
                                               t['title'].startswith('Extension checks failed:'))], process)
                    result['title'] = pages[0]['title']
                    if result['title'] != 'Extension checks passed':
                        raise AssertionError(result['title'])
                    result['passed'] = True
                finally:
                    stop_browser(process)
    except Exception as error:
        result['error'] = str(error)
        raise
    finally:
        args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
