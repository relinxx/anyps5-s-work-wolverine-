"""Run the verified local test without the Codex desktop or a debugger."""
import argparse
import csv
import ctypes
import datetime
import hashlib
import json
from pathlib import Path
import runpy
import shutil
import subprocess
import sys

from windows_capture_resources import MemoryStatus, kernel

RESEARCH = Path(__file__).resolve().parent
ROOT = RESEARCH.parents[1]
RUNS = ROOT / 'work/wolverine-michon-runs'
LAUNCHER = RESEARCH / 'launch_michon.py'
OPTIONS = [
    '--seconds', '900', '--host-import-mib', '8192', '--prefer-host-imports',
    '--host-import-window-mib', '128', '--trace-host-import', '--no-whole-range-import',
    '--device-heap-mirrors', '--device-heap-descriptor-mirrors', '--reuse-device-heap-mirrors',
    '--file-backed-direct-memory', '--trim-mirror-free-mib', '4096',
    '--trim-source-range', '--trim-refresh-source-range',
    '--trim-pressure-source-free-mib', '2048', '--heap-mirror-mib', '8192',
    '--stream-heap-writeback-mib', '64',
    '--trace-gpu-memory', '--memory-ledger', '--allocation-audit', '--trace-ir-reclaim',
    '--prepare-workers', '2', '--prepare-queue-mib', '64', '--prepare-queue-entries', '32',
    '--resource-cache-entries', '32', '--dispatch-cache-entries', '64',
    '--recorder-kept-mib', '128', '--staging-pool-mib', '64', '--host-buffer-pool-mib', '64',
    '--texture-cache-mib', '1024', '--unit-shadow-mib', '1024',
    '--heap-optimize-free-mib', '4096', '--demand-registration-prepare',
    '--dump-every', '16', '--frame-limit', '128',
]


def preflight():
    manifest = json.loads((RESEARCH / 'michon-staged-runtime.json').read_text())
    runtime = Path(manifest['runtime'])
    for entry in manifest['files']:
        with (runtime / entry['path']).open('rb') as stream:
            actual = hashlib.file_digest(stream, 'sha256').hexdigest()
        if actual != entry['sha256']:
            raise RuntimeError(f"Runtime verification failed: {entry['path']}")
    tasks = subprocess.check_output(
        ['tasklist.exe', '/FI', 'IMAGENAME eq wolverine.exe', '/FO', 'CSV', '/NH'],
        text=True, encoding='utf-8', errors='replace')
    if any(row and row[0].lower() == 'wolverine.exe' for row in csv.reader(tasks.splitlines())):
        raise RuntimeError('Wolverine is already running. Close that run before starting another.')
    status = MemoryStatus(length=ctypes.sizeof(MemoryStatus))
    if not kernel.GlobalMemoryStatusEx(ctypes.byref(status)):
        raise ctypes.WinError(ctypes.get_last_error())
    disk = shutil.disk_usage(runtime).free
    gib = 1024 ** 3
    print(f'Available RAM: {status.physical_available / gib:.2f} GiB', flush=True)
    print(f'Available commit: {status.commit_available / gib:.2f} GiB', flush=True)
    print(f'Free disk: {disk / gib:.2f} GiB', flush=True)
    if status.physical_available < 4 * gib:
        raise RuntimeError('Less than 4 GiB RAM is available. Close unused apps and try again.')
    if status.commit_available < 4 * gib:
        raise RuntimeError('Less than 4 GiB commit headroom is available. Close unused apps and try again.')
    if disk < 12 * gib:
        raise RuntimeError('Less than 12 GiB disk is free. This logged run needs more headroom.')
    return {
        'checked_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'physical_available': status.physical_available,
        'commit_available': status.commit_available, 'disk_free': disk,
        'launcher_options': OPTIONS,
        'driver_sha256': next(e['sha256'] for e in manifest['files']
                              if Path(e['path']).name == 'libSceAgcDriver.prx'),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--check-only', action='store_true', help='Verify files and RAM without starting the game')
    args = parser.parse_args()
    checked = preflight()
    if args.check_only:
        subprocess.run([sys.executable, str(LAUNCHER), *OPTIONS, '--check-only'], check=True)
        print('Verification passed. No game was started.')
        return 0
    (RESEARCH / 'standalone-preflight.json').write_text(json.dumps(checked, indent=2))
    previous = {p.name for p in RUNS.iterdir() if p.is_dir()} if RUNS.exists() else set()
    print('\nStarting the current local fixes. Codex does not need to be open.', flush=True)
    print('Leave this console open while playing. Close the game window to finish.', flush=True)
    print('This test stops at 15 minutes, or sooner below 1 GiB RAM, 8 GiB disk,', flush=True)
    print('or 256 MiB commit headroom. A guard stop is recorded separately from a crash.', flush=True)
    sys.argv = [str(LAUNCHER), *OPTIONS]
    try:
        runpy.run_path(str(LAUNCHER), run_name='__main__')
    finally:
        new_runs = sorted(p for p in RUNS.iterdir() if p.is_dir() and p.name not in previous)
        if new_runs:
            run = new_runs[-1]
            (RESEARCH / 'WOLVERINE_LAST_RUN.txt').write_text(str(run) + '\n')
            print(f'\nSaved logs: {run}', flush=True)
            if (run / 'run.json').exists():
                result = json.loads((run / 'run.json').read_text())
                print(f"Stop reason: {json.dumps(result.get('limit_stop'))}", flush=True)
                print(f"Natural exit: {result.get('natural_exit')}; exit code: {result.get('exit_code')}", flush=True)
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f'\nCould not complete the run: {error}', file=sys.stderr, flush=True)
        raise SystemExit(1)
