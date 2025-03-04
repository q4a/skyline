// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <asm-generic/unistd.h>
#include <fcntl.h>
#include "memory.h"
#include "types/KProcess.h"

namespace skyline::kernel {
    MemoryManager::MemoryManager(const DeviceState &state) : state(state) {}

    MemoryManager::~MemoryManager() {
        if (base.valid() && !base.empty())
            munmap(reinterpret_cast<void *>(base.data()), base.size());
    }

    constexpr size_t RegionAlignment{1ULL << 21}; //!< The minimum alignment of a HOS memory region
    constexpr size_t CodeRegionSize{4ULL * 1024 * 1024 * 1024}; //!< The assumed maximum size of the code region (4GiB)

    void MemoryManager::InitializeVmm(memory::AddressSpaceType type) {
        size_t baseSize{};
        switch (type) {
            case memory::AddressSpaceType::AddressSpace32Bit:
            case memory::AddressSpaceType::AddressSpace32BitNoReserved:
                throw exception("32-bit address spaces are not supported");

            case memory::AddressSpaceType::AddressSpace36Bit: {
                addressSpace = span<u8>{reinterpret_cast<u8 *>(0), 1ULL << 36};
                baseSize = 0x78000000 + 0x180000000 + 0x78000000 + 0x180000000;
                throw exception("36-bit address spaces are not supported"); // Due to VMM base being forced at 0x800000 and it being used by ART
            }

            case memory::AddressSpaceType::AddressSpace39Bit: {
                addressSpace = span<u8>{reinterpret_cast<u8 *>(0), 1ULL << 39};
                baseSize = CodeRegionSize + 0x1000000000 + 0x180000000 + 0x80000000 + 0x1000000000;
                break;
            }

            default:
                throw exception("VMM initialization with unknown address space");
        }

        // Search for a suitable carveout in host AS to fit the guest AS inside of
        std::ifstream mapsFile("/proc/self/maps");
        std::string maps((std::istreambuf_iterator<char>(mapsFile)), std::istreambuf_iterator<char>());
        size_t line{}, start{1ULL << 35}, alignedStart{1ULL << 35}; // Qualcomm KGSL (Kernel Graphic Support Layer/Kernel GPU driver) maps below 35-bits, reserving it causes KGSL to go OOM
        do {
            auto end{util::HexStringToInt<u64>(std::string_view(maps.data() + line, sizeof(u64) * 2))};
            if (end < start)
                continue;
            if (end - start > baseSize + (alignedStart - start)) { // We don't want to overflow if alignedStart > start
                base = span<u8>{reinterpret_cast<u8 *>(alignedStart), baseSize};
                break;
            }

            start = util::HexStringToInt<u64>(std::string_view(maps.data() + maps.find_first_of('-', line) + 1, sizeof(u64) * 2));
            alignedStart = util::AlignUp(start, RegionAlignment);
            if (alignedStart + baseSize > addressSpace.size()) // We don't want to map past the end of the address space
                break;
        } while ((line = maps.find_first_of('\n', line)) != std::string::npos && line++);

        if (!base.valid())
            throw exception("Cannot find a suitable carveout for the guest address space");

        memoryFd = static_cast<int>(syscall(__NR_memfd_create, "HOS-AS", MFD_CLOEXEC)); // We need to use memfd directly as ASharedMemory doesn't always use it while we depend on it for FreeMemory (using FALLOC_FL_PUNCH_HOLE) to work
        if (memoryFd == -1)
            throw exception("Failed to create memfd for guest address space: {}", strerror(errno));

        if (ftruncate(memoryFd, static_cast<off_t>(base.size())) == -1)
            throw exception("Failed to resize memfd for guest address space: {}", strerror(errno));

        auto result{mmap(reinterpret_cast<void *>(base.data()), base.size(), PROT_WRITE, MAP_FIXED | MAP_SHARED, memoryFd, 0)};
        if (result == MAP_FAILED)
            throw exception("Failed to mmap guest address space: {}", strerror(errno));

        chunks = {
            ChunkDescriptor{
                .ptr = addressSpace.data(),
                .size = static_cast<size_t>(base.data() - addressSpace.data()),
                .state = memory::states::Reserved,
            },
            ChunkDescriptor{
                .ptr = base.data(),
                .size = base.size(),
                .state = memory::states::Unmapped,
            },
            ChunkDescriptor{
                .ptr = base.end().base(),
                .size = addressSpace.size() - reinterpret_cast<u64>(base.end().base()),
                .state = memory::states::Reserved,
            }};
    }

    void MemoryManager::InitializeRegions(span<u8> codeRegion) {
        if (!util::IsAligned(codeRegion.data(), RegionAlignment))
            throw exception("Non-aligned code region was used to initialize regions: 0x{:X} - 0x{:X}", codeRegion.data(), codeRegion.end().base());

        switch (addressSpace.size()) {
            case 1UL << 36: {
                code = span<u8>{reinterpret_cast<u8 *>(0x800000), 0x78000000};
                if (code.data() > codeRegion.data() || (code.end().base() < codeRegion.end().base()))
                    throw exception("Code mapping larger than 36-bit code region");
                alias = span<u8>{code.end().base(), 0x180000000};
                stack = span<u8>{alias.end().base(), 0x78000000};
                tlsIo = stack; //!< TLS/IO is shared with Stack on 36-bit
                heap = span<u8>{stack.end().base(), 0x180000000};
                break;
            }

            case 1UL << 39: {
                code = span<u8>{base.data(), util::AlignUp(codeRegion.size(), RegionAlignment)};
                alias = span<u8>{code.end().base(), 0x1000000000};
                heap = span<u8>{alias.end().base(), 0x180000000};
                stack = span<u8>{heap.end().base(), 0x80000000};
                tlsIo = span<u8>{stack.end().base(), 0x1000000000};
                break;
            }

            default:
                throw exception("Regions initialized without VMM initialization");
        }

        auto newSize{code.size() + alias.size() + stack.size() + heap.size() + ((addressSpace.size() == 1UL << 39) ? tlsIo.size() : 0)};
        if (newSize > base.size())
            throw exception("Guest VMM size has exceeded host carveout size: 0x{:X}/0x{:X} (Code: 0x{:X}/0x{:X})", newSize, base.size(), code.size(), CodeRegionSize);
        if (newSize != base.size())
            munmap(base.end().base(), newSize - base.size());

        if (codeRegion.size() > code.size())
            throw exception("Code region ({}) is smaller than mapped code size ({})", code.size(), codeRegion.size());

        Logger::Debug("Region Map:\nVMM Base: 0x{:X}\nCode Region: 0x{:X} - 0x{:X} (Size: 0x{:X})\nAlias Region: 0x{:X} - 0x{:X} (Size: 0x{:X})\nHeap Region: 0x{:X} - 0x{:X} (Size: 0x{:X})\nStack Region: 0x{:X} - 0x{:X} (Size: 0x{:X})\nTLS/IO Region: 0x{:X} - 0x{:X} (Size: 0x{:X})", base.data(), code.data(), code.end().base(), code.size(), alias.data(), alias.end().base(), alias.size(), heap.data(), heap.end().base(), heap.size(), stack.data(), stack.end().base(), stack.size(), tlsIo.data(), tlsIo.end().base(), tlsIo.size());
    }

    span<u8> MemoryManager::CreateMirror(span<u8> mapping) {
        if (mapping.data() < base.data() || mapping.end().base() > base.end().base())
            throw exception("Mapping is outside of VMM base: 0x{:X} - 0x{:X}", mapping.data(), mapping.end().base());

        auto offset{static_cast<size_t>(mapping.data() - base.data())};
        if (!util::IsPageAligned(offset) || !util::IsPageAligned(mapping.size()))
            throw exception("Mapping is not aligned to a page: 0x{:X}-0x{:X} (0x{:X})", mapping.data(), mapping.end().base(), offset);

        auto mirror{mmap(nullptr, mapping.size(), PROT_READ | PROT_WRITE | PROT_EXEC, MAP_SHARED, memoryFd, static_cast<off_t>(offset))};
        if (mirror == MAP_FAILED)
            throw exception("Failed to create mirror mapping at 0x{:X}-0x{:X} (0x{:X}): {}", mapping.data(), mapping.end().base(), offset, strerror(errno));

        return span<u8>{reinterpret_cast<u8 *>(mirror), mapping.size()};
    }

    span<u8> MemoryManager::CreateMirrors(const std::vector<span<u8>> &regions) {
        size_t totalSize{};
        for (const auto &region : regions)
            totalSize += region.size();

        auto mirrorBase{mmap(nullptr, totalSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)}; // Reserve address space for all mirrors
        if (mirrorBase == MAP_FAILED)
            throw exception("Failed to create mirror base: {} (0x{:X} bytes)", strerror(errno), totalSize);

        size_t mirrorOffset{};
        for (const auto &region : regions) {
            if (region.data() < base.data() || region.end().base() > base.end().base())
                throw exception("Mapping is outside of VMM base: 0x{:X} - 0x{:X}", region.data(), region.end().base());

            auto offset{static_cast<size_t>(region.data() - base.data())};
            if (!util::IsPageAligned(offset) || !util::IsPageAligned(region.size()))
                throw exception("Mapping is not aligned to a page: 0x{:X}-0x{:X} (0x{:X})", region.data(), region.end().base(), offset);

            auto mirror{mmap(reinterpret_cast<u8 *>(mirrorBase) + mirrorOffset, region.size(), PROT_READ | PROT_WRITE | PROT_EXEC, MAP_SHARED | MAP_FIXED, memoryFd, static_cast<off_t>(offset))};
            if (mirror == MAP_FAILED)
                throw exception("Failed to create mirror mapping at 0x{:X}-0x{:X} (0x{:X}): {}", region.data(), region.end().base(), offset, strerror(errno));

            mirrorOffset += region.size();
        }

        if (mirrorOffset != totalSize)
            throw exception("Mirror size mismatch: 0x{:X} != 0x{:X}", mirrorOffset, totalSize);

        return span<u8>{reinterpret_cast<u8 *>(mirrorBase), totalSize};
    }

    void MemoryManager::FreeMemory(span<u8> memory) {
        if (memory.data() < base.data() || memory.end().base() > base.end().base())
            throw exception("Mapping is outside of VMM base: 0x{:X} - 0x{:X}", memory.data(), memory.end().base());

        auto offset{static_cast<size_t>(memory.data() - base.data())};
        if (!util::IsPageAligned(offset) || !util::IsPageAligned(memory.size()))
            throw exception("Mapping is not aligned to a page: 0x{:X}-0x{:X} (0x{:X})", memory.data(), memory.end().base(), offset);

        // We need to use fallocate(FALLOC_FL_PUNCH_HOLE) to free the backing memory rather than madvise(MADV_REMOVE) as the latter fails when the memory doesn't have write permissions, we generally need to free memory after reprotecting it to disallow accesses between the two calls which would cause UB
        if (fallocate(*memoryFd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<off_t>(offset), static_cast<off_t>(memory.size())) != 0)
            throw exception("Failed to free memory at 0x{:X}-0x{:X} (0x{:X}): {}", memory.data(), memory.end().base(), offset, strerror(errno));
    }

    void MemoryManager::InsertChunk(const ChunkDescriptor &chunk) {
        std::unique_lock lock(mutex);

        auto upper{std::upper_bound(chunks.begin(), chunks.end(), chunk.ptr, [](const u8 *ptr, const ChunkDescriptor &chunk) -> bool { return ptr < chunk.ptr; })};
        if (upper == chunks.begin())
            throw exception("InsertChunk: Chunk inserted outside address space: 0x{:X} - 0x{:X} and 0x{:X} - 0x{:X}", upper->ptr, upper->ptr + upper->size, chunk.ptr, chunk.ptr + chunk.size);

        upper = chunks.erase(upper, std::upper_bound(upper, chunks.end(), chunk.ptr + chunk.size, [](const u8 *ptr, const ChunkDescriptor &chunk) -> bool { return ptr < chunk.ptr + chunk.size; }));
        if (upper != chunks.end() && upper->ptr < chunk.ptr + chunk.size) {
            auto end{upper->ptr + upper->size};
            upper->ptr = chunk.ptr + chunk.size;
            upper->size = static_cast<size_t>(end - upper->ptr);
        }

        auto lower{std::prev(upper)};
        if (lower->ptr == chunk.ptr && lower->size == chunk.size) {
            lower->state = chunk.state;
            lower->permission = chunk.permission;
            lower->attributes = chunk.attributes;
        } else if (lower->ptr + lower->size > chunk.ptr + chunk.size) {
            auto lowerExtension{*lower};
            lowerExtension.ptr = chunk.ptr + chunk.size;
            lowerExtension.size = static_cast<size_t>((lower->ptr + lower->size) - lowerExtension.ptr);

            lower->size = static_cast<size_t>(chunk.ptr - lower->ptr);
            if (lower->size) {
                upper = chunks.insert(upper, lowerExtension);
                chunks.insert(upper, chunk);
            } else {
                auto lower2{std::prev(lower)};
                if (chunk.IsCompatible(*lower2) && lower2->ptr + lower2->size >= chunk.ptr) {
                    lower2->size = static_cast<size_t>(chunk.ptr + chunk.size - lower2->ptr);
                    upper = chunks.erase(lower);
                } else {
                    *lower = chunk;
                }
                upper = chunks.insert(upper, lowerExtension);
            }
        } else if (chunk.IsCompatible(*lower) && lower->ptr + lower->size >= chunk.ptr) {
            lower->size = static_cast<size_t>(chunk.ptr + chunk.size - lower->ptr);
        } else {
            if (lower->ptr + lower->size > chunk.ptr)
                lower->size = static_cast<size_t>(chunk.ptr - lower->ptr);
            if (upper != chunks.end() && chunk.IsCompatible(*upper) && chunk.ptr + chunk.size >= upper->ptr) {
                upper->ptr = chunk.ptr;
                upper->size = chunk.size + upper->size;
            } else {
                chunks.insert(upper, chunk);
            }
        }
    }

    std::optional<ChunkDescriptor> MemoryManager::Get(void *ptr) {
        std::shared_lock lock(mutex);

        auto chunk{std::upper_bound(chunks.begin(), chunks.end(), reinterpret_cast<u8 *>(ptr), [](const u8 *ptr, const ChunkDescriptor &chunk) -> bool { return ptr < chunk.ptr; })};
        if (chunk-- != chunks.begin())
            if ((chunk->ptr + chunk->size) > ptr)
                return std::make_optional(*chunk);

        return std::nullopt;
    }

    size_t MemoryManager::GetUserMemoryUsage() {
        std::shared_lock lock(mutex);
        size_t size{};
        for (const auto &chunk : chunks)
            if (chunk.state == memory::states::Heap)
                size += chunk.size;
        return size + code.size() + state.process->mainThreadStack->guest.size();
    }

    size_t MemoryManager::GetSystemResourceUsage() {
        std::shared_lock lock(mutex);
        constexpr size_t KMemoryBlockSize{0x40};
        return std::min(static_cast<size_t>(state.process->npdm.meta.systemResourceSize), util::AlignUp(chunks.size() * KMemoryBlockSize, PAGE_SIZE));
    }
}
