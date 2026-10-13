# Wolverine / AnyPS5 archive status

Archived 2026-10-13 from `wolverine/testing`, based on upstream HEAD
`913a29b20d4c21bfe080819bedfa1106cf313394`. Emulator source remains GPL-2.0;
retain the upstream license notices. This is an experimental emulator branch,
not a game distribution and not a verified playable build.

## Latest result

Run `20261013T063835Z-6c32224e` enabled `APS5_GPU_HEAP_COMPARE=1`.
GPU comparison cut recorded heap snapshot capture plus write-back comparison
from approximately **737 seconds** in the preceding full 900-second run to
**45.60 seconds** in this 253-second test (different durations/progress, not a
controlled speed ratio). The guest `RenderFrameIndex` reached **1103** at
approximately **240 seconds**. Across **3798 completed GPU comparisons**, the
diagnostics reported **0 changed blocks**. There were no logged BDA faults.

The collector stopped the game at **253.36 seconds** when available RAM fell
below its **1 GiB reserve**; this was a guard stop, not a natural game crash.
The last ledger showed imported host memory around **6.9 GB**
(6882 MiB, about 6.72 GiB) and a game working set around **11.5 GB**
(11510 MiB, about 11.24 GiB). Pipeline creation calls totalled about **2.07 s**.
The highest logged presentation milestone was 900, with no confirmed presents
past 1050. Guest RenderFrameIndex and presented-frame count are separate;
neither these results nor focused test passes establish playable gameplay.

**Conclusion:** 16 GB of system RAM is insufficient for the loading-phase
test with the present implementation/settings. **32 GB is recommended for
continued testing**, without a guarantee that it solves remaining FPS,
shader correctness or compatibility issues.

## Changes preserved

The archive includes BDA mirror write permissions and the cached heap-mirror
permission fix; `bdaWrites` shader artifacts and serialization; bounded,
byte-exact snapshot reuse; GPU-compared write-back (`APS5_GPU_HEAP_COMPARE`,
`HeapCompare.comp`); Vulkan allocation/pipeline accounting and memory ledgers;
zero-mirror pilots, source-page trimming and import census/zero audit;
protection of critical/write-bearing imports during allocation recovery;
IR reclamation; invocation disk cache/capture infrastructure; preparation
queue budgets; and focused memory/coherence/GPU regression tests.

## Next steps

1. Avoid reference captures for mirrors the GPU demonstrably never writes,
   using a sound write-ownership/proof mechanism. Do not merely suppress GPU
   writes or BDA faults. Preserve exact changed-byte write-back, concurrent CPU
   changes, guest registration lifetime, and protected 112/121 heap coherence.
2. Continue on 32 GB RAM, distinguish actual loading/menu presents from guest
   frame counters, and measure CPU/GPU frame time before further FPS work.
3. Resolve any remaining shader validation/compatibility failures. Compare
   warmed repeated runs at equal frames with matching initial free RAM.

## Rebuild

The previous Windows build used **MinGW-w64 GCC 15.2**, **CMake/Ninja**,
Release mode, C++20, testing enabled and SPIRV-Tools enabled. The toolchain,
build outputs, game inputs and runtimes are intentionally **not archived**.
Re-download a compatible MinGW-w64 GCC 15.2 toolchain plus CMake/Ninja/Python
and dependencies. Initialize the upstream submodules before configuring:

```powershell
git switch wolverine/testing
git submodule update --init --recursive
# Prepend the re-downloaded MinGW/CMake/Ninja bin directory to PATH.
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DANYPS5_ENABLE_SPIRV_TOOLS=ON
cmake --build build --target libs -j 2
cmake --build build --target agc_host_import_retire_tests agc_memory_accounting_tests agc_driver_device_heap_mirrors_tests agc_import_first_heap_tests agc_write_tracking_tests agc_zero_mirror_policy_tests -j 2
ctest --test-dir build --output-on-failure -R '^(agc_host_import_retire|agc_memory_accounting|agc_driver_device_heap_mirrors|agc_driver_device_heap_stream_writeback|agc_import_first_heap|agc_write_tracking|agc_zero_mirror_policy)$'
```

The notes preserve only the explicitly requested historical scripts. They
reference the former workspace/runtime and additional local helpers and
manifests that are intentionally omitted. They are **not a self-contained
game launcher** after cleanup. Recreate the supporting harness locally,
adjust paths, and supply legally obtained game inputs separately. No game
files, raw run logs, frames, shader/pipeline/invocation captures or binaries
are included. `HANDOVER.txt` and the RAM handover are older context;
this STATUS supersedes their result/next-action sections.

## Launch configuration used

The local invocation was equivalent to
`run_zero_mirror_pilot.ps1 -Only10d -GpuHeapCompare -RunSeconds 900`
after closing other apps. The effective settings below are copied as text
configuration, without captured game data. The collector kept its 8 GiB
disk, 1 GiB physical-RAM and 256 MiB commit reserves.

```text
APS5_AMM_RANGE_GIB=11
APS5_BACKGROUND_PIPELINE_WORDS=1000000
APS5_BINDLESS_STORAGE_TABLES=1
APS5_DEMAND_REGISTRATION_PREPARE=1
APS5_DEPTH_CLEAR_DRAWS=1
APS5_DEVICE_HEAP_DESCRIPTOR_MIRRORS=1
APS5_DEVICE_HEAP_MIRRORS=1
APS5_DISPATCH_CACHE_ENTRIES=64
APS5_DRAW_SNAPSHOT_MAX_MIB=32
APS5_DUMP_FRAMES=256
APS5_DUMP_FRAMES_EVERY=64
APS5_DUMP_SCALE=4
APS5_EAGER_LINEAR_WRITEBACK=1
APS5_FILE_BACKED_DIRECT_MEMORY=1
APS5_GPU_CHECKPOINTS=1
APS5_GPU_HEAP_COMPARE=1
APS5_HEAP_MIRROR_MIB=8192
APS5_HEAP_OPTIMIZE_FREE_MIB=4096
APS5_HOST_BUFFER_POOL_MIB=64
APS5_HOST_IMPORT_MIB=8192
APS5_HOST_IMPORT_WINDOW_MIB=128
APS5_IGNORE_CONSERVATIVE_Z=1
APS5_IGNORE_POPS=1
APS5_IMPORT_ZERO_AUDIT=1
APS5_KEEP_11E_HOST_IMPORT=1
APS5_MEMORY_LEDGER=1
APS5_MEMORY_LEDGER_INTERVAL_MS=10000
APS5_NO_RESULT_MEMO=1
APS5_NO_SNAPSHOT_CHECK=1
APS5_NO_WHOLE_RANGE_IMPORT=1
APS5_NULL_UNRESOLVED_IMAGES=1
APS5_PREFER_HOST_IMPORTS=1
APS5_PREPARE_QUEUE_ENTRIES=32
APS5_PREPARE_QUEUE_MIB=64
APS5_PREPARE_WORKERS=2
APS5_PROFILE_DRAW=1
APS5_PS_STATE_FROM_SHADER=1
APS5_RECORDER_KEPT_MIB=128
APS5_RESERVE_HONOR_HINT=1
APS5_RESOURCE_CACHE_ENTRIES=32
APS5_REUSE_DEVICE_HEAP_MIRRORS=1
APS5_STAGING_POOL_MIB=64
APS5_STREAM_HEAP_WRITEBACK_MIB=64
APS5_SUBROUTINE_CALLS=1
APS5_TEXTURE_CACHE_MIB=1024
APS5_TOLERATE_DISPATCH=1
APS5_TRACE_GPU_MEMORY=1
APS5_TRACE_HOST_IMPORT=1
APS5_TRACE_IR_RECLAIM=1
APS5_TRACE_MEMORY=1
APS5_TRIM_MIRROR_WORKING_SET_MIB=4096
APS5_TRIM_PRESSURE_SOURCE_FREE_MIB=2048
APS5_TRIM_REFRESH_SOURCE_RANGE=1
APS5_TRIM_SOURCE_RANGE=1
APS5_UNIT_SHADOW_MIB=1024
APS5_WIDE_BINDLESS=1
APS5_ZERO_MIRROR_PILOTS=10d
```
