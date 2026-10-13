#pragma once

#include <cstdint>

// APS5_MEMORY_LEDGER=1: cheap process/system/GPU totals every 10 s, at most once per second
// under pressure. APS5_MEMORY_LEDGER_FULL=1 additionally enables the expensive resident-page
// and Windows-heap census, at most every 10 s. Full census is unsuitable for performance runs.
// Neither mode changes allocation, cache or rendering behaviour.
namespace AgcDriver::Graphics::MemoryLedger {

bool Enabled();
// CPU bytes held by sampled-texture snapshot entries (guest surface copies kept for comparison).
void AddTextureSnapshotBytes(std::int64_t delta);
// Prints a sample if enabled and its periodic or pressure cadence permits.
void Report();
void ReportHostImports(const char* reason);

}
