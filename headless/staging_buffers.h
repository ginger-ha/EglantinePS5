// SPDX-License-Identifier: GPL-3.0-or-later
// Staging buffers of Eden's pool on the console (tools/prepare-vulkan-port.py, vulkan_staging_buffer_pool.cpp).
//
// Eden makes a staging buffer the next power of two above its transfer: 512 MiB for a transfer of
// 341 MiB. On the console a buffer that size is one block of the pool the CPU shares with the GPU,
// so 171 MiB were asked for and never used, and the request fails where the pool's largest free
// block is smaller (a Switch 2 title was refused 512 MiB with 480 MiB free). Above 16 MiB a buffer
// the CPU writes or reads is now its transfer rounded up to 16 MiB. Up to 16 MiB, and for buffers
// only the GPU uses, it stays Eden's power of two.
//
// Eden keeps a buffer the GPU is done with for the next transfer of the same power of two. Above
// 16 MiB it can be smaller than that transfer, so a kept buffer is handed out only when it holds
// the transfer, and one too small for a new transfer is let go before the new one is made.
//
// When the console refuses a buffer, every kept one the GPU is done with is let go and the buffer
// is asked for once more.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace Eden::StagingBuffers {
inline constexpr std::uint64_t kExactAbove = std::uint64_t{4} << 20;
inline constexpr std::uint64_t kStep = std::uint64_t{2} << 20;

// Whether a transfer of `size` bytes gets a buffer of its own size.
constexpr bool Exact(std::uint64_t size, bool /*mapped*/ = true) noexcept {
    return size > kExactAbove;
}

// The size of the buffer made for `size` bytes; `log2` is Eden's Log2Ceil of it.
constexpr std::uint64_t Size(std::uint64_t size, unsigned log2, bool exact) noexcept {
    return exact ? (size + kStep - 1) / kStep * kStep : std::uint64_t{1} << log2;
}

// What a kept buffer holds: its recorded size, its mapping's size, or Eden's power of two.
template <class Entry>
constexpr std::uint64_t Holds(const Entry& entry) noexcept {
    if (entry.size != 0) return entry.size;
    if (entry.mapped_span.size() != 0) return entry.mapped_span.size();
    return std::uint64_t{1} << entry.log2_level;
}

constexpr std::uint64_t Holds(std::uint64_t mapped_bytes, unsigned log2) noexcept {
    return mapped_bytes != 0 ? mapped_bytes : std::uint64_t{1} << log2;
}

struct Released {
    std::size_t buffers = 0;
    std::uint64_t bytes = 0;
    Released& operator+=(const Released& other) noexcept {
        buffers += other.buffers;
        bytes += other.bytes;
        return *this;
    }
};

// Lets go of the kept buffers of one level that `drop` picks. `level` is the pool's
// StagingBuffers: its entries and the two places in them where its next search and its next
// release start, which stay inside the list.
template <class Level, class Drop>
Released ReleaseFrom(Level& level, Drop&& drop) {
    Released released;
    auto& entries = level.entries;
    const auto kept = std::remove_if(entries.begin(), entries.end(), [&](const auto& entry) {
        if (!drop(entry)) return false;
        ++released.buffers;
        released.bytes += Holds(entry);
        return true;
    });
    entries.erase(kept, entries.end());
    if (level.delete_index >= entries.size()) level.delete_index = 0;
    if (level.iterate_index > entries.size()) level.iterate_index = 0;
    return released;
}
} // namespace Eden::StagingBuffers
