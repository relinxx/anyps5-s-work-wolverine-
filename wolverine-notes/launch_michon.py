"""Separate, bounded Michon baseline. No debugger and no inherited experimental flags."""
import argparse
import hashlib
import json
import os
import shutil
import datetime
from pathlib import Path
from capture_run import capture
from summarize_michon_run import summarize

root = Path(__file__).resolve().parents[2]
research = root / 'work/research'
parser = argparse.ArgumentParser()
parser.add_argument('--seconds', type=int, default=120)
parser.add_argument('--commit-grace-seconds', type=float, default=0)
parser.add_argument('--host-import-mib', type=int)
parser.add_argument('--prefer-host-imports', action='store_true')
parser.add_argument('--host-import-window-mib', type=int)
parser.add_argument('--trace-host-import', action='store_true')
parser.add_argument('--import-pressure-free-mib', type=int,
                    help='Retire idle small/GPU-read-only imports below this RAM headroom; preserves large writable heaps')
parser.add_argument('--heap-optimize-free-mib', type=int,
                    help='Ask Windows to decommit unused LFH caches every 15 seconds under RAM/commit pressure')
parser.add_argument('--no-whole-range-import', action='store_true')
parser.add_argument('--device-heap-mirrors', action='store_true')
parser.add_argument('--device-heap-descriptor-mirrors', action='store_true')
parser.add_argument('--reuse-device-heap-mirrors', action='store_true')
parser.add_argument('--file-backed-direct-memory', action='store_true')
parser.add_argument('--trim-mirror-free-mib', type=int)
parser.add_argument('--trim-source-range', action='store_true',
                    help='Trim only uploaded guest pages, preserving locked pages under shared ownership')
parser.add_argument('--trim-refresh-source-range', action='store_true',
                    help='Also stream large mirror refreshes through targeted source-page trimming')
parser.add_argument('--trim-refresh-free-mib', type=int)
parser.add_argument('--trim-pressure-source-free-mib', type=int,
                    help='Under RAM pressure, trim only fallback heap source pages every 15 seconds')
parser.add_argument('--stream-heap-writeback-mib', type=int,
                    help='Bound large fallback heap write-back snapshots using exact chunked temporary storage')
parser.add_argument('--check-only', action='store_true', help='Validate settings and runtime without starting the game')
parser.add_argument('--capture-pipeline', action='store_true', help='Diagnostic only: save a large pipeline and stop the run immediately afterward')
parser.add_argument('--capture-invocation', action='store_true', help='Save exact live shader inputs before costly materialization, then stop')
parser.add_argument('--trace-pipeline', action='store_true', help='Save large pipeline inputs while continuing normal execution')
parser.add_argument('--disk-invocations', action='store_true', help='Reuse byte-identical offline prepared large compute invocations')
parser.add_argument('--capture-physical-reserve-mib', type=int, choices=[512, 1024], default=1024)
parser.add_argument('--dump-every', type=int, default=16)
parser.add_argument('--heap-mirror-mib', type=int)
parser.add_argument('--trace-gpu-memory', action='store_true')
parser.add_argument('--memory-ledger', action='store_true')
parser.add_argument('--memory-ledger-interval-ms', type=int, default=10000,
                    help='Periodic census/ledger interval (1000..60000 ms)')
parser.add_argument('--memory-ledger-full', action='store_true',
                    help='Walk the resident working set at the configured ledger interval; diagnostic and intrusive')
parser.add_argument('--import-zero-audit', action='store_true',
                    help='Scan live host imports once at the first import census (or below 1 GiB free RAM)')
parser.add_argument('--zero-mirror-pilots', action='store_true',
                    help='Opt in to GPU-zeroed mirrors for only 0x10d0000000 and 0x11e0000000; leaves 0x1120000000 imported')
parser.add_argument('--zero-mirror-10d-only', action='store_true',
                    help='GPU-zeroed mirror for only 0x10d0000000; 0x11e0000000 keeps the normal import-first path')
parser.add_argument('--gpu-heap-compare', action='store_true',
                    help='Large device heap write-backs keep their reference on the GPU and download only changed 64 KiB blocks')
parser.add_argument('--preserve-11e-imports', action='store_true',
                    help='10d-only experiment: protect 11e imports from budget/LRU retirement; registration retirement remains enabled')
parser.add_argument('--allocation-audit', action='store_true', help='Sample actual pagefile sizes and bounded NVIDIA cache metadata; no debugger')
parser.add_argument('--trace-ir-reclaim', action='store_true')
parser.add_argument('--sync-shader-prepare', action='store_true')
parser.add_argument('--demand-registration-prepare', action='store_true',
                    help='Defer registration artifacts to ABI resolution or first invocation, without queued unused work')
parser.add_argument('--prepare-workers', type=int)
parser.add_argument('--prepare-queue-mib', type=int)
parser.add_argument('--prepare-queue-entries', type=int)
parser.add_argument('--resource-cache-entries', type=int)
parser.add_argument('--dispatch-cache-entries', type=int)
parser.add_argument('--recorder-kept-mib', type=int)
parser.add_argument('--staging-pool-mib', type=int)
parser.add_argument('--host-buffer-pool-mib', type=int)
parser.add_argument('--texture-cache-mib', type=int)
parser.add_argument('--unit-shadow-mib', type=int)
parser.add_argument('--frame-limit', type=int, default=128)
args = parser.parse_args()
if args.zero_mirror_pilots and args.zero_mirror_10d_only:
    parser.error('Choose one zero-mirror pilot mode')
if args.preserve_11e_imports and not args.zero_mirror_10d_only:
    parser.error('11e import preservation requires the 10d-only pilot')
if sum([args.capture_pipeline, args.capture_invocation, args.trace_pipeline]) > 1:
    parser.error('Capture one diagnostic stage at a time')
if args.capture_physical_reserve_mib != 1024 and not (args.capture_pipeline or (args.disk_invocations and args.trace_pipeline)):
    parser.error('The lower reserve is only for the bounded pipeline capture experiment')
if args.import_pressure_free_mib is not None and not 1024 <= args.import_pressure_free_mib <= 8192:
    parser.error('Import pressure headroom must be 1024..8192 MiB')
if args.heap_optimize_free_mib is not None and not 1024 <= args.heap_optimize_free_mib <= 8192:
    parser.error('Heap optimization headroom must be 1024..8192 MiB')
for name, value in [('texture cache', args.texture_cache_mib), ('unit shadow', args.unit_shadow_mib)]:
    if value is not None and not 64 <= value <= 4096:
        parser.error(f'{name} budget must be 64..4096 MiB')
if not 1 <= args.dump_every <= 256:
    parser.error('Frame capture interval must be 1..256')
if not 1 <= args.frame_limit <= 256:
    parser.error('Frame capture count must be 1..256')
if args.trim_refresh_free_mib is not None and (not args.device_heap_mirrors or not 256 <= args.trim_refresh_free_mib <= 4096):
    parser.error('Ongoing working-set checks require device mirrors and a threshold of 256..4096 MiB')
if args.host_buffer_pool_mib is not None and not 0 <= args.host_buffer_pool_mib <= 4096:
    parser.error('Host buffer retention budget must be 0..4096 MiB')
if not 1 <= args.seconds <= 900:
    parser.error('Duration must be 1..900 seconds; memory and disk guards remain enabled')
if not 1000 <= args.memory_ledger_interval_ms <= 60000:
    parser.error('Memory ledger interval must be 1000..60000 ms')
if args.memory_ledger_full and not args.memory_ledger:
    parser.error('Full memory census requires --memory-ledger')
if not 0 <= args.commit_grace_seconds <= 2:
    parser.error('Commit grace must be 0..2 seconds')
if args.host_import_mib is not None and not 256 <= args.host_import_mib <= 8192:
    parser.error('Host import limit must be 256..8192 MiB')
if args.host_import_window_mib is not None and (args.host_import_window_mib < 1 or
        args.host_import_window_mib > 1024 or args.host_import_window_mib & (args.host_import_window_mib - 1)):
    parser.error('Host import window must be a power of two from 1..1024 MiB')
if args.trim_mirror_free_mib is not None and (not args.device_heap_mirrors or not 256 <= args.trim_mirror_free_mib <= 4096):
    parser.error('Mirror trimming requires device mirrors and a threshold of 256..4096 MiB')
if args.trim_source_range and args.trim_mirror_free_mib is None:
    parser.error('Source range trimming requires an initial mirror trim threshold')
if args.trim_refresh_source_range and not args.trim_source_range:
    parser.error('Refresh source trimming requires source range trimming')
if args.trim_pressure_source_free_mib is not None and (not args.device_heap_mirrors or
        not args.trim_source_range or not 256 <= args.trim_pressure_source_free_mib <= 4096):
    parser.error('Pressure source trimming requires device mirrors, source range trimming and 256..4096 MiB')
if args.trim_pressure_source_free_mib is not None and args.trim_refresh_free_mib is not None:
    parser.error('Select targeted source pressure trimming or whole-process pressure trimming, not both')
if args.stream_heap_writeback_mib is not None and (not args.device_heap_descriptor_mirrors or
        not 1 <= args.stream_heap_writeback_mib <= 4096):
    parser.error('Bounded heap write-back requires heap descriptor mirrors and 1..4096 MiB')
if args.heap_mirror_mib is not None and not 256 <= args.heap_mirror_mib <= 12288:
    parser.error('Heap mirror limit must be 256..12288 MiB')
if args.recorder_kept_mib is not None and not 1 <= args.recorder_kept_mib <= 4096:
    parser.error('Recorder retained-byte budget must be 1..4096 MiB')
if args.staging_pool_mib is not None and not 1 <= args.staging_pool_mib <= 4096:
    parser.error('Device buffer pool budget must be 1..4096 MiB')
for name, value, maximum in [('workers', args.prepare_workers, 64),
        ('queue MiB', args.prepare_queue_mib, 4096), ('queue entries', args.prepare_queue_entries, 4096),
        ('resource cache entries', args.resource_cache_entries, 4096), ('dispatch cache entries', args.dispatch_cache_entries, 8192)]:
    if value is not None and not 1 <= value <= maximum:
        parser.error(f'Preparation {name} must be 1..{maximum}')
if args.sync_shader_prepare and any(v is not None for v in
        [args.prepare_workers, args.prepare_queue_mib, args.prepare_queue_entries]):
    parser.error('Synchronous preparation cannot use background queue options')
manifest = json.loads((research / 'michon-staged-runtime.json').read_text())
runtime = Path(manifest['runtime'])
for entry in manifest['files']:
    file = runtime / entry['path']
    with file.open('rb') as stream:
        actual = hashlib.file_digest(stream, 'sha256').hexdigest()
    if actual != entry['sha256']:
        raise RuntimeError(f'Staged artifact changed: {file}')
settings = json.loads((research / 'michon_settings.json').read_text())
settings.update(APS5_DUMP_FRAMES=str(args.frame_limit), APS5_DUMP_SCALE='4', APS5_DUMP_FRAMES_EVERY=str(args.dump_every),
                APS5_PROFILE_DRAW='1')
if args.host_import_mib is not None:
    settings['APS5_HOST_IMPORT_MIB'] = str(args.host_import_mib)
if args.prefer_host_imports:
    settings['APS5_PREFER_HOST_IMPORTS'] = '1'
if args.host_import_window_mib is not None:
    settings['APS5_HOST_IMPORT_WINDOW_MIB'] = str(args.host_import_window_mib)
if args.trace_host_import:
    settings['APS5_TRACE_HOST_IMPORT'] = '1'
if args.no_whole_range_import:
    settings['APS5_NO_WHOLE_RANGE_IMPORT'] = '1'
if args.device_heap_mirrors:
    settings['APS5_DEVICE_HEAP_MIRRORS'] = '1'
if args.device_heap_descriptor_mirrors:
    if not args.device_heap_mirrors:
        parser.error('Heap descriptor reuse requires device heap mirrors')
    settings['APS5_DEVICE_HEAP_DESCRIPTOR_MIRRORS'] = '1'
if args.reuse_device_heap_mirrors:
    if not args.device_heap_mirrors:
        parser.error('Mirror reuse requires device heap mirrors')
    settings['APS5_REUSE_DEVICE_HEAP_MIRRORS'] = '1'
if args.file_backed_direct_memory:
    settings['APS5_FILE_BACKED_DIRECT_MEMORY'] = '1'
    settings['APS5_TRACE_MEMORY'] = '1'
if args.trim_mirror_free_mib is not None:
    settings['APS5_TRIM_MIRROR_WORKING_SET_MIB'] = str(args.trim_mirror_free_mib)
if args.trim_source_range:
    settings['APS5_TRIM_SOURCE_RANGE'] = '1'
if args.trim_refresh_source_range:
    settings['APS5_TRIM_REFRESH_SOURCE_RANGE'] = '1'
if args.trim_refresh_free_mib is not None:
    settings['APS5_TRIM_REFRESH_WORKING_SET_MIB'] = str(args.trim_refresh_free_mib)
if args.trim_pressure_source_free_mib is not None:
    settings['APS5_TRIM_PRESSURE_SOURCE_FREE_MIB'] = str(args.trim_pressure_source_free_mib)
if args.stream_heap_writeback_mib is not None:
    settings['APS5_STREAM_HEAP_WRITEBACK_MIB'] = str(args.stream_heap_writeback_mib)
if args.heap_mirror_mib is not None:
    settings['APS5_HEAP_MIRROR_MIB'] = str(args.heap_mirror_mib)
if args.trace_gpu_memory:
    settings['APS5_TRACE_GPU_MEMORY'] = '1'
if args.import_pressure_free_mib is not None:
    settings['APS5_IMPORT_PRESSURE_FREE_MIB'] = str(args.import_pressure_free_mib)
if args.heap_optimize_free_mib is not None:
    settings['APS5_HEAP_OPTIMIZE_FREE_MIB'] = str(args.heap_optimize_free_mib)
if args.memory_ledger:
    settings['APS5_MEMORY_LEDGER'] = '1'
    settings['APS5_MEMORY_LEDGER_INTERVAL_MS'] = str(args.memory_ledger_interval_ms)
if args.memory_ledger_full:
    settings['APS5_MEMORY_LEDGER_FULL'] = '1'
if args.import_zero_audit:
    settings['APS5_IMPORT_ZERO_AUDIT'] = '1'
if args.zero_mirror_pilots or args.zero_mirror_10d_only:
    settings['APS5_ZERO_MIRROR_PILOTS'] = '10d' if args.zero_mirror_10d_only else '1'
    settings['APS5_DEVICE_HEAP_MIRRORS'] = '1'
    settings['APS5_PREFER_HOST_IMPORTS'] = '1'
    settings['APS5_IMPORT_ZERO_AUDIT'] = '1'
if args.trace_ir_reclaim:
    settings['APS5_TRACE_IR_RECLAIM'] = '1'
if args.preserve_11e_imports:
    settings['APS5_KEEP_11E_HOST_IMPORT'] = '1'
if args.gpu_heap_compare:
    settings['APS5_GPU_HEAP_COMPARE'] = '1'
if args.sync_shader_prepare:
    settings['APS5_SYNC_SHADER_PREPARE'] = '1'
if args.demand_registration_prepare:
    settings['APS5_DEMAND_REGISTRATION_PREPARE'] = '1'
for key, value in [('APS5_PREPARE_WORKERS', args.prepare_workers),
        ('APS5_PREPARE_QUEUE_MIB', args.prepare_queue_mib),
        ('APS5_PREPARE_QUEUE_ENTRIES', args.prepare_queue_entries),
        ('APS5_RESOURCE_CACHE_ENTRIES', args.resource_cache_entries),
        ('APS5_DISPATCH_CACHE_ENTRIES', args.dispatch_cache_entries)]:
    if value is not None:
        settings[key] = str(value)
if args.recorder_kept_mib is not None:
    settings['APS5_RECORDER_KEPT_MIB'] = str(args.recorder_kept_mib)
if args.staging_pool_mib is not None:
    settings['APS5_STAGING_POOL_MIB'] = str(args.staging_pool_mib)
if args.host_buffer_pool_mib is not None:
    settings['APS5_HOST_BUFFER_POOL_MIB'] = str(args.host_buffer_pool_mib)
if args.texture_cache_mib is not None:
    settings['APS5_TEXTURE_CACHE_MIB'] = str(args.texture_cache_mib)
if args.unit_shadow_mib is not None:
    settings['APS5_UNIT_SHADOW_MIB'] = str(args.unit_shadow_mib)
pipeline_directory = None
invocation_directory = None
if args.disk_invocations:
    settings['APS5_DISK_INVOCATION_MIN_CODE_WORDS'] = '20000'
if args.capture_invocation:
    invocation_directory = research / ('invocation-inputs-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ'))
    settings['APS5_CAPTURE_INVOCATION_DIR'] = str(invocation_directory)
    settings['APS5_CAPTURE_INVOCATION'] = '11d0376e00'
if args.capture_pipeline or args.trace_pipeline:
    pipeline_directory = research / ('pipeline-inputs-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ'))
    settings['APS5_DUMP_PIPELINE_DIR' if args.capture_pipeline else 'APS5_TRACE_PIPELINE_DIR'] = str(pipeline_directory)
if args.check_only:
    print(json.dumps({'runtime_verified': str(runtime), 'settings': settings}, indent=2))
    raise SystemExit(0)
# Clear old experiments in this launcher only, never system or user settings.
inherited = {k: os.environ.pop(k) for k in list(os.environ)
             if k.startswith(('APS5_', 'ANYPS5_', 'VK_'))}
metadata = dict(manifest, purpose='Michon wolverine/testing baseline',
                removed_inherited_settings=inherited, debugger_attached=False)
metadata['allocation_audit_enabled'] = args.allocation_audit
if invocation_directory is not None:
    invocation_directory.mkdir()
    metadata['invocation_capture_directory'] = str(invocation_directory)
if pipeline_directory is not None:
    pipeline_directory.mkdir()
    metadata['pipeline_capture_directory'] = str(pipeline_directory)
# Keep old captures separate so each run's frames have unambiguous provenance.
old_frames = list(runtime.glob('frame_*.bmp'))
if old_frames:
    archive = research / ('michon-frames-before-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ'))
    archive.mkdir()
    for frame in old_frames:
        shutil.move(str(frame), archive / frame.name)
    metadata['previous_frames_archived'] = str(archive)
run, result = capture([str(runtime / 'wolverine.exe')], runtime,
    root / 'work/wolverine-michon-runs', 'benchmark', settings, metadata,
    procdump=None, max_seconds=args.seconds, min_free_bytes=8*1024**3,
    min_commit_bytes=256*1024**2, min_physical_bytes=args.capture_physical_reserve_mib*1024**2,
    commit_grace_seconds=args.commit_grace_seconds, allocation_audit=args.allocation_audit,
    stop_marker=('complete shader invocation saved: ' if args.capture_invocation else
                 'pipeline inputs written to ' if args.capture_pipeline else None))
frames = run / 'frames'
frames.mkdir(exist_ok=True)
frame_hashes = {}
for frame in runtime.glob('frame_*.bmp'):
    copied = frames / frame.name
    shutil.copy2(frame, copied)
    with copied.open('rb') as stream:
        frame_hashes[frame.name] = hashlib.file_digest(stream, 'sha256').hexdigest()
(run / 'frame-hashes.json').write_text(json.dumps(frame_hashes, indent=2))
summarize(run)
print(json.dumps({'run': str(run), 'exit_code': result['exit_code'],
                  'limit_stop': result['limit_stop'], 'capture_complete': result['capture_complete']}, default=str))
