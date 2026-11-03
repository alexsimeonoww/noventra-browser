"""Isolated Linux browser PSS measurements. No third-party Python packages."""
import argparse
import functools
import http.server
import json
import os
import re
from pathlib import Path
import signal
import statistics
import subprocess
import tempfile
import threading
import time
import urllib.parse
import urllib.request


def parse_stat(text):
    pid = int(text.split(' ', 1)[0])
    fields = text[text.rfind(')') + 2:].split()
    return dict(pid=pid, ppid=int(fields[1]), session=int(fields[3]),
                starttime=int(fields[19]), cpu_ticks=int(fields[11]) + int(fields[12]))


def sample_processes(root_pid, proc_root=Path('/proc')):
    records = {}
    for entry in proc_root.iterdir():
        if not entry.name.isdecimal():
            continue
        try:
            record = parse_stat((entry / 'stat').read_text())
            records[record['pid']] = record
        except (OSError, ValueError, IndexError):
            continue
    if root_pid not in records:
        raise ProcessLookupError(f'Browser root {root_pid} exited')
    # Include reparented children in the dedicated session and descendants
    # that created their own session. Never select processes by executable name.
    selected = {root_pid} | {pid for pid, r in records.items() if r['session'] == root_pid}
    while True:
        more = {pid for pid, r in records.items() if r['ppid'] in selected}
        if more <= selected:
            break
        selected |= more
    totals = dict(pss_kib=0, swap_pss_kib=0)
    unreadable = []
    identities = {}
    for pid in sorted(selected):
        try:
            fields = {}
            for line in (proc_root / str(pid) / 'smaps_rollup').read_text().splitlines():
                if line.startswith(('Pss:', 'SwapPss:', 'Rss:', 'Private_Clean:', 'Private_Dirty:',
                                    'Pss_Anon:', 'Pss_File:', 'Pss_Shmem:')):
                    key, value, _unit = line.split()
                    fields[key] = int(value)
            command = (proc_root / str(pid) / 'cmdline').read_bytes().decode(errors='replace').replace('\0', ' ')
            if not command.strip():
                raise ProcessLookupError('Process command line unavailable')
            after = parse_stat((proc_root / str(pid) / 'stat').read_text())
            if after['starttime'] != records[pid]['starttime']:
                raise ProcessLookupError('PID reused')
            totals['pss_kib'] += fields['Pss:']
            totals['swap_pss_kib'] += fields['SwapPss:']
            role = re.search(r'--type=([^ ]+)', command)
            subtype = re.search(r'--utility-sub-type=([^ ]+)', command)
            private_kib = (fields['Private_Clean:'] + fields['Private_Dirty:']
                           if 'Private_Clean:' in fields and 'Private_Dirty:' in fields else None)
            identities[str(pid)] = dict(starttime=after['starttime'], cpu_ticks=after['cpu_ticks'],
                                       pss_kib=fields['Pss:'], rss_kib=fields.get('Rss:'),
                                       private_kib=private_kib,
                                       pss_anon_kib=fields.get('Pss_Anon:'),
                                       pss_file_kib=fields.get('Pss_File:'),
                                       pss_shmem_kib=fields.get('Pss_Shmem:'),
                                       role=role[1] if role else 'browser/helper',
                                       subtype=subtype[1] if subtype else None,
                                       extension_process='--extension-process' in command)
        except (OSError, ValueError, KeyError, IndexError):
            unreadable.append(pid)
    return dict(**(totals if not unreadable else dict(pss_kib=None, swap_pss_kib=None)),
                complete=not unreadable, process_count=len(selected),
                unreadable_pids=unreadable, processes=identities)


def sample_process_roots(root_pids, proc_root=Path('/proc')):
    """Combine distinct owned browser sessions without hiding missing accounting."""
    root_pids = list(root_pids)
    if (not root_pids or len(set(root_pids)) != len(root_pids)
            or any(type(pid) is not int or pid <= 0 for pid in root_pids)):
        raise ValueError('Require distinct positive browser root PIDs')
    samples = [sample_processes(pid, proc_root) for pid in root_pids]
    selected = set()
    identities = {}
    unreadable = []
    for sample in samples:
        members = set(sample['processes']) | {str(pid) for pid in sample['unreadable_pids']}
        if selected & members:
            raise ValueError('Browser roots have overlapping process trees')
        selected |= members
        identities.update(sample['processes'])
        unreadable.extend(sample['unreadable_pids'])
    complete = all(sample['complete'] for sample in samples)
    return dict(root_pids=root_pids, complete=complete,
                pss_kib=sum(sample['pss_kib'] for sample in samples) if complete else None,
                swap_pss_kib=sum(sample['swap_pss_kib'] for sample in samples) if complete else None,
                process_count=len(selected), unreadable_pids=sorted(unreadable),
                processes=identities)


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *_args):
        pass


def summarize_cpu(samples, hz):
    if hz <= 0:
        raise ValueError('CPU clock frequency must be positive')
    percentages = []
    total_ticks = 0
    observed_seconds = 0
    for before, after in zip(samples, samples[1:]):
        a, b = before['processes'], after['processes']
        elapsed = after['elapsed_s'] - before['elapsed_s']
        # A process exit/replacement loses CPU history; expose coverage rather
        # than treating missing work as zero consumption.
        if (not before['complete'] or not after['complete'] or elapsed <= 0
                or not a or a.keys() != b.keys()
                or any(a[p]['starttime'] != b[p]['starttime'] for p in a)):
            continue
        deltas = [b[p]['cpu_ticks'] - a[p]['cpu_ticks'] for p in a]
        if any(delta < 0 for delta in deltas):
            continue
        delta = sum(deltas)
        percentages.append(100 * delta / hz / elapsed)
        total_ticks += delta
        observed_seconds += elapsed
    return dict(idle_cpu_percent_one_core=statistics.median(percentages) if percentages else None,
                idle_cpu_mean_percent_one_core=(100 * total_ticks / hz / observed_seconds
                                                if observed_seconds else None),
                idle_cpu_observed_seconds=observed_seconds,
                idle_cpu_valid_intervals=len(percentages),
                idle_cpu_total_intervals=max(0, len(samples) - 1))


def request(port, path, method='GET'):
    req = urllib.request.Request(f'http://127.0.0.1:{port}{path}', method=method)
    with urllib.request.urlopen(req, timeout=3) as response:
        data = response.read()
    try:
        return json.loads(data)
    except json.JSONDecodeError:
        return data.decode()


def stop_browser(process):
    # The launched process is a session/group leader. Never kill by browser name.
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=10)
    except ProcessLookupError:
        process.wait()
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=5)


def run_once(args, tab_count, run, fixture_url, root):
    with tempfile.TemporaryDirectory(prefix='profile-', dir=root) as temp:
        profile = Path(temp)
        if getattr(args, 'profile_local_state', None):
            (profile / 'Local State').write_text(json.dumps(args.profile_local_state))
        command = [str(args.browser), f'--user-data-dir={profile}', '--no-first-run',
                   '--no-default-browser-check', '--remote-debugging-address=127.0.0.1',
                   '--remote-debugging-port=0', '--window-size=1280,800']
        if not args.headed:
            command.append('--headless=new')
        command.append('about:blank')
        log_path = root / f'browser-{tab_count}-{run}.log'
        with log_path.open('w') as log:
            start = time.monotonic()
            process = subprocess.Popen(command, stdout=log, stderr=log, start_new_session=True)
            try:
                active_port = profile / 'DevToolsActivePort'
                deadline = start + 40
                while not active_port.exists():
                    if process.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError(f'Browser failed to start; inspect {log_path}')
                    time.sleep(.1)
                port = int(active_port.read_text().splitlines()[0])
                debug_ready_ms = (time.monotonic() - start) * 1000
                time.sleep(5)  # Allow browser-specific welcome targets to finish opening.
                initial = request(port, '/json/list')
                if tab_count == 0:
                    blank = request(port, '/json/new?about%3Ablank', 'PUT')
                    for target in initial:
                        if target['type'] == 'page':
                            request(port, '/json/close/' + target['id'])
                    initial = [blank]
                for tab in range(tab_count):
                    page_url = fixture_url
                    if getattr(args, 'multi_site', False):
                        page_url = page_url.replace('127.0.0.1', f'site{tab}.localhost')
                    url = urllib.parse.quote(f'{page_url}?tab={tab}', safe='')
                    request(port, '/json/new?' + url, 'PUT')
                if tab_count:
                    for target in initial:
                        if target['type'] == 'page':
                            request(port, '/json/close/' + target['id'])
                    deadline = time.monotonic() + 30
                    while True:
                        pages = [t for t in request(port, '/json/list') if t['type'] == 'page']
                        if len(pages) == tab_count and all(t['title'] == 'Noventra fixture ready' for t in pages):
                            break
                        if time.monotonic() > deadline:
                            raise RuntimeError('Fixture pages did not all finish loading')
                        time.sleep(.2)
                time.sleep(args.settle)
                pages = [t for t in request(port, '/json/list') if t['type'] == 'page']
                if len(pages) != max(1, tab_count) or (tab_count == 0 and pages[0]['url'] != 'about:blank'):
                    raise RuntimeError('Unexpected browser tabs invalidate the memory workload')
                samples = []
                for _ in range(args.samples):
                    if process.poll() is not None:
                        raise RuntimeError('Browser exited during measurement')
                    sample = sample_processes(process.pid)
                    sample['elapsed_s'] = time.monotonic() - start
                    samples.append(sample)
                    time.sleep(1)
                if not all(s['complete'] for s in samples):
                    raise RuntimeError(f'Incomplete memory accounting: {samples}')
                cpu = summarize_cpu(samples, os.sysconf('SC_CLK_TCK'))
                # Inspect after collecting samples: debugger attachment must not
                # change the memory samples it is meant to help interpret.
                page_states = None
                if getattr(args, 'record_page_state', False):
                    probe = Path(__file__).with_name('page_state_probe.mjs')
                    page_states = json.loads(subprocess.check_output(
                        ['node', str(probe), str(port)], text=True, timeout=60))
                return dict(tabs=tab_count, run=run, debugging_ready_ms=debug_ready_ms,
                            pss_mib=statistics.median(s['pss_kib'] / 1024 for s in samples),
                            **cpu,
                            command=command, samples=samples, page_states=page_states)
            finally:
                stop_browser(process)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--tabs', type=int, nargs='+', default=[0, 1, 5, 20])
    parser.add_argument('--runs', type=int, default=3)
    parser.add_argument('--settle', type=float, default=8)
    parser.add_argument('--samples', type=int, default=5)
    parser.add_argument('--headed', action='store_true')
    parser.add_argument('--record-page-state', action='store_true',
                        help='Record visibility and window/viewport state after RAM samples')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.runs < 1 or args.samples < 2 or args.settle < 0 or any(n < 0 for n in args.tabs):
        parser.error('Require runs >= 1, samples >= 2, settle >= 0 and tabs >= 0')
    args.browser = args.browser.resolve(strict=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    root = args.output.parent.resolve()
    fixtures = root / 'fixtures'
    fixtures.mkdir(exist_ok=True)
    (fixtures / 'index.html').write_text('''<!doctype html><meta charset="utf-8">
<style>body{font:16px sans-serif;background:#eee}article{padding:12px;margin:8px;background:white}</style>
<main></main><script>
const m=document.querySelector('main');
for(let i=0;i<500;i++){const a=document.createElement('article');a.textContent='Noventra controlled content '+i;m.append(a)}
document.title='Noventra fixture ready';</script>''')
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), functools.partial(QuietHandler, directory=str(fixtures)))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    results = dict(browser_version=subprocess.check_output([str(args.browser), '--version'], text=True).strip(),
                   mode='headed' if args.headed else 'headless',
                   workload='500 static DOM cards per same-origin local page; 0 tabs means one blank page',
                   limitations=['No real-world website or extension equivalence claims',
                                'Debugging-ready time is not time to visible first window',
                                'Process PSS does not fully account for GPU device memory',
                                'Fresh profiles; repeated starts are not controlled cold-cache starts'],
                   records=[])
    try:
        for run in range(args.runs):
            for tabs in args.tabs:
                result = run_once(args, tabs, run, f'http://127.0.0.1:{server.server_port}/index.html', root)
                results['records'].append(result)
                args.output.write_text(json.dumps(results, indent=2) + '\n')
                print(f'run={run+1} tabs={tabs} PSS={result["pss_mib"]:.1f} MiB', flush=True)
    finally:
        server.shutdown()


if __name__ == '__main__':
    main()
