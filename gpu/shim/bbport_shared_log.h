// driveclub: BB_SHARED_LOG (bbport_shared_log.cpp): GPU write ranges in CustomHeap(SystemShared), logged once each.
#pragma once
#include <cstdint>

namespace BbSharedLog {
bool Enabled();
/// GPU thread (BufferCache::StampGpuWrite).
void GpuWrite(std::uint64_t addr, std::uint64_t size, std::uint64_t writer);
} // namespace BbSharedLog
