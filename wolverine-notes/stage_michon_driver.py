import datetime
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[2]
research = root / 'work/research'
manifest_path = research / 'michon-staged-runtime.json'
manifest = json.loads(manifest_path.read_text())
runtime = Path(manifest['runtime'])
parser = argparse.ArgumentParser()
parser.add_argument('--library', choices=['libSceAgcDriver.prx', 'libSceVideoOut.prx', 'libkernel.prx', 'libc.prx'], default='libSceAgcDriver.prx')
args = parser.parse_args()
name = args.library
target = runtime / 'libs' / name
source = root / 'work/michon-build/core/libs/libs' / name
unpatched = source.parent / 'unpatched' / name
if unpatched.stat().st_mtime_ns > source.stat().st_mtime_ns:
    raise RuntimeError('Final NID-patched driver is older than its build input; build target libs before staging')
def digest(p):
    with p.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()
entry = next(e for e in manifest['files'] if Path(e['path']) == Path('libs') / name)
if digest(target) != entry['sha256']:
    raise RuntimeError('Existing driver differs from recorded artifact')
if digest(source) == entry['sha256']:
    print(json.dumps({'driver_sha256': entry['sha256'], 'unchanged': True}))
    raise SystemExit(0)
backup = research / ('michon-driver-backup-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ'))
backup.mkdir()
shutil.copy2(target, backup / name)
shutil.copy2(manifest_path, backup / manifest_path.name)
shutil.copy2(source, target)
entry['sha256'] = digest(target)
if entry['sha256'] != digest(source):
    raise RuntimeError('Driver staging verification failed')
checkout = root / 'work/AnyPS5-michon'
paths = subprocess.check_output(['git','-C',str(checkout),'ls-files','-m','-o','--exclude-standard'],text=True).splitlines()
manifest['local_source_hashes'] = {p:digest(checkout/p) for p in paths if (checkout/p).is_file()}
manifest['diagnostic_driver_backup'] = str(backup)
manifest_path.write_text(json.dumps(manifest,indent=2))
print(json.dumps({'library': name, 'sha256':entry['sha256'],'backup':str(backup)}))
