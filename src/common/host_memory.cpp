// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32

#include <array>
#include <iterator>
#include <thread>
#include "common/container/unordered_map.h"
#include <boost/icl/separate_interval_set.hpp>
#include <windows.h>
#include "common/dynamic_library.h"

#else // ^^^ Windows ^^^ vvv POSIX vvv

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <boost/icl/interval_set.hpp>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "common/scope_exit.h"

#if defined(__linux__)
#include <sys/random.h>
#elif defined(__APPLE__)
#include <sys/types.h>
#include <sys/random.h>
#include <mach/vm_map.h>
#include <mach/mach.h>
#elif defined(__FreeBSD__)
#include <sys/shm.h>
#elif defined(__OPENORBIS__)
#include <orbis/libkernel.h>
#endif

// FreeBSD
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
// Solaris 11 and illumos
#ifndef MAP_ALIGNED_SUPER
#define MAP_ALIGNED_SUPER 0
#endif
// macOS
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#endif // ^^^ POSIX ^^^

#include <cstdlib>
#include <mutex>
#include <random>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/div_ceil.h"
#include "common/free_region_manager.h"
#include "common/host_memory.h"
#include "common/logging.h"

#if defined(__ANDROID__) && __ANDROID_API__ < 30
#include <sys/syscall.h>
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
static int memfd_create(const char* name, unsigned int flags) {
    return syscall(__NR_memfd_create, name, flags);
}
#endif

namespace Common {

[[maybe_unused]] constexpr size_t PageAlignment = 0x1000;
[[maybe_unused]] constexpr size_t HugePageSize = 0x200000;

#ifdef _WIN32

// Manually imported for MinGW compatibility
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_COALESCE_PLACEHOLDERS
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif

using PFN_CreateFileMapping2 = _Ret_maybenull_ HANDLE(WINAPI*)(
    _In_ HANDLE File, _In_opt_ SECURITY_ATTRIBUTES* SecurityAttributes, _In_ ULONG DesiredAccess,
    _In_ ULONG PageProtection, _In_ ULONG AllocationAttributes, _In_ ULONG64 MaximumSize,
    _In_opt_ PCWSTR Name,
    _Inout_updates_opt_(ParameterCount) MEM_EXTENDED_PARAMETER* ExtendedParameters,
    _In_ ULONG ParameterCount);

using PFN_VirtualAlloc2 = _Ret_maybenull_ PVOID(WINAPI*)(
    _In_opt_ HANDLE Process, _In_opt_ PVOID BaseAddress, _In_ SIZE_T Size,
    _In_ ULONG AllocationType, _In_ ULONG PageProtection,
    _Inout_updates_opt_(ParameterCount) MEM_EXTENDED_PARAMETER* ExtendedParameters,
    _In_ ULONG ParameterCount);

using PFN_MapViewOfFile3 = _Ret_maybenull_ PVOID(WINAPI*)(
    _In_ HANDLE FileMapping, _In_opt_ HANDLE Process, _In_opt_ PVOID BaseAddress,
    _In_ ULONG64 Offset, _In_ SIZE_T ViewSize, _In_ ULONG AllocationType, _In_ ULONG PageProtection,
    _Inout_updates_opt_(ParameterCount) MEM_EXTENDED_PARAMETER* ExtendedParameters,
    _In_ ULONG ParameterCount);

using PFN_UnmapViewOfFile2 = BOOL(WINAPI*)(_In_ HANDLE Process, _In_ PVOID BaseAddress,
                                           _In_ ULONG UnmapFlags);

// The backing section (14 GiB for Switch 2 layouts) used to be created with SEC_COMMIT: it charged its whole size
// against the commit limit at boot, used or not, and with too little commit free boot failed (round 23). It is now
// created with SEC_RESERVE and its pages are committed as the kernel hands them out (HostMemory::MarkUsed); the
// handler below commits any other backing page on first access. EDEN_COMMIT_UPFRONT=1 restores SEC_COMMIT.
static bool LazyCommit() {
    static const bool lazy = [] {
        const char* value = std::getenv("EDEN_COMMIT_UPFRONT");
        return value == nullptr || value[0] == '0';
    }();
    return lazy;
}

static bool IsCommitError(DWORD error) {
    return error == ERROR_COMMITMENT_LIMIT || error == ERROR_NOT_ENOUGH_MEMORY ||
           error == ERROR_NO_SYSTEM_RESOURCES || error == ERROR_COMMITMENT_MINIMUM ||
           error == ERROR_OUTOFMEMORY;
}

/// Commits [base, base + length) of reserved memory (a SEC_RESERVE view or a reserved buffer). A full commit limit
/// often clears once Windows has grown the page file, so commit errors are retried for up to 30 s.
static bool CommitRange(u8* base, size_t length) {
    for (int attempt = 0;; ++attempt) {
        if (VirtualAlloc(base, length, MEM_COMMIT, PAGE_READWRITE) != nullptr) {
            if (attempt > 0) {
                LOG_WARNING(HW_Memory, "Committed {:#x} bytes of backing memory after {} retries", length,
                            attempt);
            }
            return true;
        }
        const DWORD error = GetLastError();
        if (!IsCommitError(error) || attempt >= 60) {
            LOG_CRITICAL(HW_Memory, "Failed to commit {:#x} bytes of backing memory at {}, error {}", length,
                         fmt::ptr(base), error);
            return false;
        }
        if (attempt == 0) {
            LOG_WARNING(HW_Memory,
                        "Committing {:#x} bytes of backing memory failed with error {}; retrying while the "
                        "page file grows",
                        length, error);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

/// Backing ranges whose reserved pages the access-violation handler may commit.
struct LazyRegion {
    std::atomic<u8*> base{};
    std::atomic<size_t> size{};
};
static std::array<LazyRegion, 4> lazy_regions{};
static std::atomic<u64> lazy_fault_commits{};
static std::atomic<size_t> lazy_committed_bytes{}; ///< committed through MarkUsed (one backing memory in practice)
static constexpr size_t LazyFaultGranule = 0x10000;

static LONG WINAPI LazyCommitHandler(PEXCEPTION_POINTERS info) {
    const EXCEPTION_RECORD* const record = info->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    u8* const address = reinterpret_cast<u8*>(record->ExceptionInformation[1]);
    for (auto& region : lazy_regions) {
        u8* const base = region.base.load(std::memory_order_acquire);
        const size_t size = region.size.load(std::memory_order_acquire);
        if (base == nullptr || address < base || address >= base + size) {
            continue;
        }
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(address, &memory, sizeof(memory)) == 0 || memory.State != MEM_RESERVE) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        const size_t offset = static_cast<size_t>(address - base) & ~(LazyFaultGranule - 1);
        const size_t length = (std::min)(LazyFaultGranule, size - offset);
        if (!CommitRange(base + offset, length)) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        const u64 count = lazy_fault_commits.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 16 || count % 4096 == 0) {
            LOG_INFO(HW_Memory, "Committed backing memory at offset {:#x} on first access ({} so far)", offset,
                     count);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void RegisterLazyRegion(u8* base, size_t size) {
    static std::once_flag once;
    std::call_once(once, [] { AddVectoredExceptionHandler(1, LazyCommitHandler); });
    for (auto& region : lazy_regions) {
        u8* expected = nullptr;
        if (region.base.load(std::memory_order_acquire) == nullptr) {
            region.size.store(size, std::memory_order_release);
            if (region.base.compare_exchange_strong(expected, base, std::memory_order_acq_rel)) {
                return;
            }
        }
    }
    LOG_CRITICAL(HW_Memory, "No free lazy commit region slot");
}

static void UnregisterLazyRegion(u8* base) {
    for (auto& region : lazy_regions) {
        u8* expected = base;
        if (base != nullptr && region.base.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel)) {
            region.size.store(0, std::memory_order_release);
        }
    }
}

template <typename T>
static void GetFuncAddress(Common::DynamicLibrary& dll, const char* name, T& pfn) {
    if (!dll.GetSymbol(name, &pfn)) {
        LOG_CRITICAL(HW_Memory, "Failed to load {}", name);
    }
}

class HostMemory::Impl {
public:
    explicit Impl(size_t backing_size_, size_t virtual_size_)
        : backing_size{backing_size_}
        , virtual_size{virtual_size_}
        , process{GetCurrentProcess()}
        , kernelbase_dll("Kernelbase")
    {}

    bool Init() {
        if (!kernelbase_dll.IsOpen()) {
            LOG_CRITICAL(HW_Memory, "Failed to load Kernelbase.dll");
            return false;
        }
        GetFuncAddress(kernelbase_dll, "CreateFileMapping2", pfn_CreateFileMapping2);
        GetFuncAddress(kernelbase_dll, "VirtualAlloc2", pfn_VirtualAlloc2);
        GetFuncAddress(kernelbase_dll, "MapViewOfFile3", pfn_MapViewOfFile3);
        GetFuncAddress(kernelbase_dll, "UnmapViewOfFile2", pfn_UnmapViewOfFile2);

        if (!pfn_CreateFileMapping2 || !pfn_VirtualAlloc2 || !pfn_MapViewOfFile3 || !pfn_UnmapViewOfFile2) {
            LOG_CRITICAL(HW_Memory, "Failed to find functions for virtual allocs");
            return false;
        }

        // Allocate backing file map (SEC_RESERVE: committed as used, see LazyCommit)
        lazy = LazyCommit();
        backing_handle = pfn_CreateFileMapping2(INVALID_HANDLE_VALUE, nullptr, FILE_MAP_WRITE | FILE_MAP_READ, PAGE_READWRITE, lazy ? SEC_RESERVE : SEC_COMMIT, backing_size, nullptr, nullptr, 0);
        if (!backing_handle) {
            last_error = GetLastError();
            LOG_CRITICAL(HW_Memory, "Failed to allocate {} MiB of backing memory, error {}", backing_size >> 20, last_error);
            return false;
        }
        // Allocate a virtual memory for the backing file map as placeholder
        backing_base = static_cast<u8*>(pfn_VirtualAlloc2(process, nullptr, backing_size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
        if (!backing_base) {
            last_error = GetLastError();
            Release();
            LOG_CRITICAL(HW_Memory, "Failed to reserve {} MiB of virtual memory, error {}", backing_size >> 20, last_error);
            return false;
        }
        // Map backing placeholder
        void* const ret = pfn_MapViewOfFile3(backing_handle, process, backing_base, 0, backing_size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
        if (ret != backing_base) {
            last_error = GetLastError();
            Release();
            LOG_CRITICAL(HW_Memory, "Failed to map {} MiB of virtual memory, error {}", backing_size >> 20, last_error);
            return false;
        }
        backing_mapped = true;
        // Allocate virtual address placeholder
        virtual_base = static_cast<u8*>(pfn_VirtualAlloc2(process, nullptr, virtual_size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
        if (!virtual_base) {
            last_error = GetLastError();
            Release();
            LOG_CRITICAL(HW_Memory, "Failed to reserve {} GiB of virtual memory, error {}", virtual_size >> 30, last_error);
            return false;
        }
        LOG_INFO(HW_Memory, "Backing memory: {} MiB section, {}", backing_size >> 20,
                 lazy ? "committed as the kernel hands pages out" : "committed up front");
        return true;
    }

    ~Impl() {
        Release();
    }

    void* Allocate(size_t size) {
        // Lazy: reserved only, committed as used (as the section). Otherwise committed, retried while the page
        // file grows.
        const bool reserve_only = LazyCommit();
        for (int attempt = 0;; ++attempt) {
            auto* ptr = VirtualAlloc(nullptr, size, MEM_RESERVE | (reserve_only ? 0 : MEM_COMMIT), PAGE_READWRITE);
            if (ptr != nullptr) {
                return ptr;
            }
            const DWORD error = GetLastError();
            if (!IsCommitError(error) || attempt >= 20) {
                LOG_CRITICAL(HW_Memory, "Failed to allocate fallback buffer with size {:#x}, error {}", size, error);
                return nullptr;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    void Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perms) {
        std::unique_lock lock{placeholder_mutex};
        if (!IsNiechePlaceholder(virtual_offset, length)) {
            Split(virtual_offset, length);
        }
        ASSERT(placeholders.find({virtual_offset, virtual_offset + length}) == placeholders.end());
        TrackPlaceholder(virtual_offset, host_offset, length);

        MapView(virtual_offset, host_offset, length);
    }

    void Unmap(size_t virtual_offset, size_t length) {
        std::scoped_lock lock{placeholder_mutex};

        // Unmap until there are no more placeholders
        while (UnmapOnePlaceholder(virtual_offset, length)) {
        }
    }

    void Protect(size_t virtual_offset, size_t length, bool read, bool write, bool execute) {
        DWORD new_flags{};
        if (read && write) {
            new_flags = PAGE_READWRITE;
        } else if (read && !write) {
            new_flags = PAGE_READONLY;
        } else if (!read && !write) {
            new_flags = PAGE_NOACCESS;
        } else {
            UNIMPLEMENTED_MSG("Protection flag combination read={} write={}", read, write);
        }
        const size_t virtual_end = virtual_offset + length;

        std::scoped_lock lock{placeholder_mutex};
        auto [it, end] = placeholders.equal_range({virtual_offset, virtual_end});
        while (it != end) {
            const size_t offset = (std::max)(it->lower(), virtual_offset);
            const size_t protect_length = (std::min)(it->upper(), virtual_end) - offset;
            DWORD old_flags{};
            if (!VirtualProtect(virtual_base + offset, protect_length, new_flags, &old_flags)) {
                LOG_CRITICAL(HW_Memory, "Failed to change virtual memory protect rules");
            }
            ++it;
        }
    }

    void EnableDirectMappedAddress() {
        // TODO
        UNREACHABLE();
    }

    const size_t backing_size; ///< Size of the backing memory in bytes
    const size_t virtual_size; ///< Size of the virtual address placeholder in bytes

    u8* backing_base{};
    u8* virtual_base{};
    DWORD last_error{}; ///< Windows error of the step Init failed at
    bool lazy{};        ///< The section is SEC_RESERVE

private:
    /// Release all resources in the object
    void Release() {
        if (!placeholders.empty()) {
            for (const auto& placeholder : placeholders) {
                if (!pfn_UnmapViewOfFile2(process, virtual_base + placeholder.lower(),
                                          MEM_PRESERVE_PLACEHOLDER)) {
                    LOG_CRITICAL(HW_Memory, "Failed to unmap virtual memory placeholder");
                }
            }
            Coalesce(0, virtual_size);
        }
        // Everything freed is cleared: Init's failure paths release, and ~Impl releases again. A second
        // VirtualFreeEx of backing_base freed the fallback buffer the constructor had just been given at the same
        // address (round 23, boot crash).
        if (virtual_base) {
            if (!VirtualFree(virtual_base, 0, MEM_RELEASE)) {
                LOG_CRITICAL(HW_Memory, "Failed to free virtual memory");
            }
            virtual_base = nullptr;
        }
        if (backing_base) {
            if (backing_mapped && !pfn_UnmapViewOfFile2(process, backing_base, MEM_PRESERVE_PLACEHOLDER)) {
                LOG_CRITICAL(HW_Memory, "Failed to unmap backing memory placeholder");
            }
            backing_mapped = false;
            if (!VirtualFreeEx(process, backing_base, 0, MEM_RELEASE)) {
                LOG_CRITICAL(HW_Memory, "Failed to free backing memory");
            }
            backing_base = nullptr;
        }
        if (backing_handle) {
            if (!CloseHandle(backing_handle)) {
                LOG_CRITICAL(HW_Memory, "Failed to free backing memory file handle");
            }
            backing_handle = nullptr;
        }
    }

    /// Unmap one placeholder in the given range (partial unmaps are supported)
    /// Return true when there are no more placeholders to unmap
    bool UnmapOnePlaceholder(size_t virtual_offset, size_t length) {
        const auto it = placeholders.find({virtual_offset, virtual_offset + length});
        const auto begin = placeholders.begin();
        const auto end = placeholders.end();
        if (it == end) {
            return false;
        }
        const size_t placeholder_begin = it->lower();
        const size_t placeholder_end = it->upper();
        const size_t unmap_begin = (std::max)(virtual_offset, placeholder_begin);
        const size_t unmap_end = (std::min)(virtual_offset + length, placeholder_end);
        ASSERT(unmap_begin >= placeholder_begin && unmap_begin < placeholder_end);
        ASSERT(unmap_end <= placeholder_end && unmap_end > placeholder_begin);

        const auto host_pointer_it = placeholder_host_pointers.find(placeholder_begin);
        ASSERT(host_pointer_it != placeholder_host_pointers.end());
        const size_t host_offset = host_pointer_it->second;

        const bool split_left = unmap_begin > placeholder_begin;
        const bool split_right = unmap_end < placeholder_end;

        if (!pfn_UnmapViewOfFile2(process, virtual_base + placeholder_begin,
                                  MEM_PRESERVE_PLACEHOLDER)) {
            LOG_CRITICAL(HW_Memory, "Failed to unmap placeholder");
        }
        // If we have to remap memory regions due to partial unmaps, we are in a data race as
        // Windows doesn't support remapping memory without unmapping first. Avoid adding any extra
        // logic within the panic region described below.

        // Panic region, we are in a data race right now
        if (split_left || split_right) {
            Split(unmap_begin, unmap_end - unmap_begin);
        }
        if (split_left) {
            MapView(placeholder_begin, host_offset, unmap_begin - placeholder_begin);
        }
        if (split_right) {
            MapView(unmap_end, host_offset + unmap_end - placeholder_begin,
                    placeholder_end - unmap_end);
        }
        // End panic region

        size_t coalesce_begin = unmap_begin;
        if (!split_left) {
            // Try to coalesce pages to the left
            coalesce_begin = it == begin ? 0 : std::prev(it)->upper();
            if (coalesce_begin != placeholder_begin) {
                Coalesce(coalesce_begin, unmap_end - coalesce_begin);
            }
        }
        if (!split_right) {
            // Try to coalesce pages to the right
            const auto next = std::next(it);
            const size_t next_begin = next == end ? virtual_size : next->lower();
            if (placeholder_end != next_begin) {
                // We can coalesce to the right
                Coalesce(coalesce_begin, next_begin - coalesce_begin);
            }
        }
        // Remove and reinsert placeholder trackers
        UntrackPlaceholder(it);
        if (split_left) {
            TrackPlaceholder(placeholder_begin, host_offset, unmap_begin - placeholder_begin);
        }
        if (split_right) {
            TrackPlaceholder(unmap_end, host_offset + unmap_end - placeholder_begin,
                             placeholder_end - unmap_end);
        }
        return true;
    }

    void MapView(size_t virtual_offset, size_t host_offset, size_t length) {
        if (!pfn_MapViewOfFile3(backing_handle, process, virtual_base + virtual_offset, host_offset,
                                length, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0)) {
            LOG_CRITICAL(HW_Memory, "Failed to map placeholder");
        }
    }

    void Split(size_t virtual_offset, size_t length) {
        if (!VirtualFreeEx(process, reinterpret_cast<LPVOID>(virtual_base + virtual_offset), length,
                           MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            LOG_CRITICAL(HW_Memory, "Failed to split placeholder");
        }
    }

    void Coalesce(size_t virtual_offset, size_t length) {
        if (!VirtualFreeEx(process, reinterpret_cast<LPVOID>(virtual_base + virtual_offset), length,
                           MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) {
            LOG_CRITICAL(HW_Memory, "Failed to coalesce placeholders");
        }
    }

    void TrackPlaceholder(size_t virtual_offset, size_t host_offset, size_t length) {
        placeholders.insert({virtual_offset, virtual_offset + length});
        placeholder_host_pointers.emplace(virtual_offset, host_offset);
    }

    void UntrackPlaceholder(boost::icl::separate_interval_set<size_t>::iterator it) {
        placeholder_host_pointers.erase(it->lower());
        placeholders.erase(it);
    }

    /// Return true when a given memory region is a "nieche" and the placeholders don't have to be
    /// split.
    bool IsNiechePlaceholder(size_t virtual_offset, size_t length) const {
        const auto it = placeholders.upper_bound({virtual_offset, virtual_offset + length});
        if (it != placeholders.end() && it->lower() == virtual_offset + length) {
            return it == placeholders.begin() ? virtual_offset == 0
                                              : std::prev(it)->upper() == virtual_offset;
        }
        return false;
    }

    HANDLE process{};        ///< Current process handle
    HANDLE backing_handle{}; ///< File based backing memory
    bool backing_mapped{};   ///< backing_base holds the view of the section (not just the placeholder)

    DynamicLibrary kernelbase_dll;
    PFN_CreateFileMapping2 pfn_CreateFileMapping2{};
    PFN_VirtualAlloc2 pfn_VirtualAlloc2{};
    PFN_MapViewOfFile3 pfn_MapViewOfFile3{};
    PFN_UnmapViewOfFile2 pfn_UnmapViewOfFile2{};

    std::mutex placeholder_mutex;                                 ///< Mutex for placeholders
    boost::icl::separate_interval_set<size_t> placeholders;       ///< Mapped placeholders
    ::Common::unordered_map<size_t, size_t> placeholder_host_pointers; ///< Placeholder backing offset
};

#elif defined(__OPENORBIS__) || defined(__managarm__)
// None of the luxuries of POSIX, all of the suffering
// For managarm: see https://github.com/managarm/managarm/issues/1370
#else // ^^^ Windows ^^^ vvv POSIX vvv

#ifndef MAP_NOCORE
#define MAP_NOCORE 0
#endif

#ifdef ARCHITECTURE_arm64

static void* ChooseVirtualBase(size_t virtual_size) {
    constexpr uintptr_t Map39BitSize = (1ULL << 39);
    constexpr uintptr_t Map36BitSize = (1ULL << 36);

    // This is not a cryptographic application, we just want something random.
    std::mt19937_64 rng;

    // We want to ensure we are allocating at an address aligned to the L2 block size.
    // For Qualcomm devices, we must also allocate memory above 36 bits.
    const size_t lower = Map36BitSize / HugePageSize;
    const size_t upper = (Map39BitSize - virtual_size) / HugePageSize;
    const size_t range = upper - lower;

    // Try up to 64 times to allocate memory at random addresses in the range.
    for (int i = 0; i < 64; i++) {
        // Calculate a possible location.
        uintptr_t hint_address = ((rng() % range) + lower) * HugePageSize;

        // Try to map.
        // Note: we may be able to take advantage of MAP_FIXED_NOREPLACE here.
        void* map_pointer =
            mmap(reinterpret_cast<void*>(hint_address), virtual_size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_NOCORE, -1, 0);

        // If we successfully mapped, we're done.
        if (reinterpret_cast<uintptr_t>(map_pointer) == hint_address) {
            return map_pointer;
        }

        // Unmap if necessary, and try again.
        if (map_pointer != MAP_FAILED) {
            munmap(map_pointer, virtual_size);
        }
    }

    return MAP_FAILED;
}

#else

static void* ChooseVirtualBase(size_t virtual_size) {
#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__) || defined(__sun__) || defined(__HAIKU__) || defined(__managarm__) || defined(__AIX__)
    void* virtual_base = mmap(nullptr, virtual_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_ALIGNED_SUPER | MAP_NOCORE, -1, 0);
    if (virtual_base != MAP_FAILED)
        return virtual_base;
#endif
    return mmap(nullptr, virtual_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_NOCORE, -1, 0);
}

#endif

#if defined(__sun__) || defined(__HAIKU__) || defined(__NetBSD__) || defined(__DragonFly__)
/// Most Unices don't have a portable shm_open (AIX, OpenBSD, NetBSD, Solaris 11, OpenIndiana)
/// Portable implementation of shm_open(SHM_ANON, ...) - roughly equivalent but without
/// OS support - may fail sporadically, beware!
static int shm_open_anon(int flags, mode_t mode) {
    char name[16] = "/shm-";
    char *const limit = name + sizeof(name) - 1;
    *limit = '\0';
    char *start = name + strlen(name);
    for (int tries = 0; tries < 4; tries++) {
        struct timespec tv;
        clock_gettime(CLOCK_REALTIME, &tv);
        unsigned long r = (unsigned long)tv.tv_sec + (unsigned long)tv.tv_nsec;
        for (char *fill = start; fill < limit; r /= 8)
            *fill++ = '0' + (r % 8);
        int fd = shm_open(name, flags, mode);
        if (fd != -1) {
            if (shm_unlink(name) == -1) {
                int tmp = errno;
                close(fd);
                errno = tmp;
                return -1;
            }
            return fd;
        }
        if (errno != EEXIST)
            break;
    }
    return -1;
}
#elif defined(__OpenBSD__)
/// Except OpenBSD which explicitly uses shm_mkstemp instead (as a more secure alternative)
static int shm_open_anon(int flags, mode_t mode) {
    char name[16] = "/shm-XXXXXXXXXX";
    int fd;
    if ((fd = shm_mkstemp(name)) == -1)
        return -1;
    if (shm_unlink(name) == -1) {
        int tmp = errno;
        close(fd);
        errno = tmp;
        return -1;
    }
    return fd;
}
#endif

class HostMemory::Impl {
public:
    explicit Impl(size_t backing_size_, size_t virtual_size_)
        : backing_size{backing_size_}
        , virtual_size{virtual_size_}
    {}

#ifdef __linux__
    bool PunchHole(size_t offset, size_t length) {
        return fd >= 0 &&
               fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<off_t>(offset),
                         static_cast<off_t>(length)) == 0;
    }
#endif

    bool Init() {
        long page_size = sysconf(_SC_PAGESIZE);
        ASSERT_MSG(page_size == 0x1000, "page size {:#x} is incompatible with 4K paging", page_size);
        // Backing memory initialization
#if defined(__sun__) || defined(__HAIKU__) || defined(__NetBSD__) || defined(__DragonFly__)
        fd = shm_open_anon(O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
#elif defined(__OpenBSD__)
        fd = shm_open_anon(O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
#elif defined(__FreeBSD__)
        fd = shm_open(SHM_ANON, O_RDWR, 0600);
#elif defined(__APPLE__) || defined(__managarm__)
        // macOS doesn't have memfd_create, use anonymous temporary file
        char template_path[] = "/tmp/eden_mem_XXXXXX";
        fd = mkstemp(template_path);
        if (fd >= 0) {
            unlink(template_path);
        }
#else
        fd = memfd_create("HostMemory", 0);
#endif
        bool use_anon = false;
        if (fd <= 0) {
            LOG_WARNING(Common_Memory, "memfd_create: {}", strerror(errno));
            use_anon = true;
        }
        if (!use_anon) {
            // Defined to extend the file with zeros
            int ret = ftruncate(fd, backing_size);
            if (ret != 0) {
                LOG_WARNING(Common_Memory, "ftruncate: {} (likely out-of-emory)", strerror(errno));
                use_anon = true;
            }
        }
        if (use_anon) {
            LOG_WARNING(Common_Memory, "Using private mappings instead of shared ones");
            backing_base = static_cast<u8*>(mmap(nullptr, backing_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_NOCORE, -1, 0));
            if (fd > 0) {
                fd = -1;
                close(fd);
            }
        } else {
            backing_base = static_cast<u8*>(mmap(nullptr, backing_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NOCORE, fd, 0));
        }
        if (backing_base == MAP_FAILED) {
            LOG_CRITICAL(HW_Memory, "mmap failed: {}", strerror(errno));
            return false;
        }

        // Virtual memory initialization
        virtual_base = virtual_map_base = static_cast<u8*>(ChooseVirtualBase(virtual_size));
        if (virtual_base == MAP_FAILED) {
            LOG_CRITICAL(HW_Memory, "mmap failed: {}", strerror(errno));
            return false;
        }
#if defined(__linux__)
        madvise(virtual_base, virtual_size, MADV_HUGEPAGE);
#endif
        free_manager.SetAddressSpace(virtual_base, virtual_size);
        return true;
    }

    ~Impl() {
        Release();
    }

    void* Allocate(size_t size) {
        auto* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (ptr == MAP_FAILED) {
            LOG_CRITICAL(HW_Memory, "Failed to allocate fallback buffer with size {:#x}, {}", size, strerror(errno));
        }
        return ptr;
    }

    void Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perms) {
        // Intersect the range with our address space.
        AdjustMap(&virtual_offset, &length);

        // We are removing a placeholder.
        free_manager.AllocateBlock(virtual_base + virtual_offset, length);

        // Deduce mapping protection flags.
        int prot_flags = PROT_NONE;
        if (True(perms & MemoryPermission::Read))
            prot_flags |= PROT_READ;
        if (True(perms & MemoryPermission::Write))
            prot_flags |= PROT_WRITE;
#ifdef ARCHITECTURE_arm64
        if (True(perms & MemoryPermission::Execute))
            prot_flags |= PROT_EXEC;
#endif
        int flags = (fd >= 0 ? MAP_SHARED : MAP_PRIVATE) | MAP_FIXED;
        void* ret = mmap(virtual_base + virtual_offset, length, prot_flags, flags, fd, host_offset);
        ASSERT_MSG(ret != MAP_FAILED, "mmap: {} {}", strerror(errno), fd);
    }

    void Unmap(size_t virtual_offset, size_t length) {
        // The method name is wrong. We're still talking about the virtual range.
        // We don't want to unmap, we want to reserve this memory.

        // Intersect the range with our address space.
        AdjustMap(&virtual_offset, &length);

        // Merge with any adjacent placeholder mappings.
        auto [merged_pointer, merged_size] =
            free_manager.FreeBlock(virtual_base + virtual_offset, length);

        void* ret = mmap(merged_pointer, merged_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        ASSERT_MSG(ret != MAP_FAILED, "mmap: {}", strerror(errno));
    }

    void Protect(size_t virtual_offset, size_t length, bool read, bool write, bool execute) {
        // Intersect the range with our address space.
        AdjustMap(&virtual_offset, &length);

        int flags = PROT_NONE;
        if (read) {
            flags |= PROT_READ;
        }
        if (write) {
            flags |= PROT_WRITE;
        }
#ifdef HAS_NCE
        if (execute) {
            flags |= PROT_EXEC;
        }
#endif
        int ret = mprotect(virtual_base + virtual_offset, length, flags);
        ASSERT_MSG(ret == 0, "mprotect failed: {}", strerror(errno));
    }

    void EnableDirectMappedAddress() {
        virtual_base = nullptr;
    }

    const size_t backing_size; ///< Size of the backing memory in bytes
    const size_t virtual_size; ///< Size of the virtual address placeholder in bytes

    u8* backing_base{reinterpret_cast<u8*>(MAP_FAILED)};
    u8* virtual_base{reinterpret_cast<u8*>(MAP_FAILED)};
    u8* virtual_map_base{reinterpret_cast<u8*>(MAP_FAILED)};

private:
    /// Release all resources in the object
    void Release() {
        if (virtual_map_base != MAP_FAILED) {
            int ret = munmap(virtual_map_base, virtual_size);
            ASSERT_MSG(ret == 0, "munmap failed: {}", strerror(errno));
        }

        if (backing_base != MAP_FAILED) {
            int ret = munmap(backing_base, backing_size);
            ASSERT_MSG(ret == 0, "munmap failed: {}", strerror(errno));
        }

        if (fd != -1) {
            int ret = close(fd);
            ASSERT_MSG(ret == 0, "close failed: {}", strerror(errno));
        }
    }

    void AdjustMap(size_t* virtual_offset, size_t* length) {
        if (virtual_base != nullptr) {
            return;
        }

        // If we are direct mapped, we want to make sure we are operating on a region
        // that is in range of our virtual mapping.
        size_t intended_start = *virtual_offset;
        size_t intended_end = intended_start + *length;
        size_t address_space_start = reinterpret_cast<size_t>(virtual_map_base);
        size_t address_space_end = address_space_start + virtual_size;

        if (address_space_start > intended_end || intended_start > address_space_end) {
            *virtual_offset = 0;
            *length = 0;
        } else {
            *virtual_offset = (std::max)(intended_start, address_space_start);
            *length = (std::min)(intended_end, address_space_end) - *virtual_offset;
        }
    }

    int fd{-1}; // memfd file descriptor, -1 is the error value of memfd_create
    FreeRegionManager free_manager{};
};

#endif // ^^^ POSIX ^^^

HostMemory::HostMemory(size_t backing_size_, size_t virtual_size_)
    : backing_size(backing_size_)
    , virtual_size(virtual_size_)
    , num_granules(Common::DivCeil(backing_size_, size_t{1} << GranuleShift))
{
    granules = std::make_unique<std::atomic<u8>[]>(num_granules);
#if defined(__OPENORBIS__) || defined(__managarm__)
    LOG_WARNING(HW_Memory, "Platform doesn't support fastmem");
    backing_base = static_cast<u8*>(mmap(nullptr, backing_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    virtual_base = nullptr;
#else
    // Try to allocate a fastmem arena.
    impl = std::make_unique<HostMemory::Impl>(AlignUp(backing_size, PageAlignment), AlignUp(virtual_size, PageAlignment) + HugePageSize);
    bool initialized = impl->Init();
#ifdef _WIN32
    // A full commit limit (ERROR_COMMITMENT_LIMIT) often clears once Windows has grown the page file.
    for (int attempt = 1; !initialized && attempt <= 10 && IsCommitError(impl->last_error); ++attempt) {
        LOG_WARNING(HW_Memory, "Fastmem arena failed with error {}; retry {} in 1 s", impl->last_error, attempt);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        impl = std::make_unique<HostMemory::Impl>(AlignUp(backing_size, PageAlignment), AlignUp(virtual_size, PageAlignment) + HugePageSize);
        initialized = impl->Init();
    }
#endif
    if (initialized) {
        backing_base = impl->backing_base;
        virtual_base = impl->virtual_base;
        if (virtual_base) {
            // Ensure the virtual base is aligned to the L2 block size.
            virtual_base = reinterpret_cast<u8*>(Common::AlignUp(uintptr_t(virtual_base), HugePageSize));
            virtual_base_offset = virtual_base - impl->virtual_base;
        }
#ifdef _WIN32
        lazy_commit = impl->lazy;
#endif
    } else {
        LOG_WARNING(HW_Memory, "Platform can support fastmem, but can't create it");
        fallback_buffer = true;
        backing_base = static_cast<u8*>(impl->Allocate(backing_size));
        virtual_base = nullptr;
        impl.reset();
#ifdef _WIN32
        lazy_commit = backing_base != nullptr && LazyCommit();
#endif
    }
#ifdef _WIN32
    if (lazy_commit) {
        RegisterLazyRegion(backing_base, backing_size);
    }
#endif
#endif
}

HostMemory::~HostMemory() {
#ifdef _WIN32
    if (lazy_commit) {
        UnregisterLazyRegion(backing_base);
    }
    if (fallback_buffer) {
        VirtualFree(backing_base, 0, MEM_RELEASE);
    }
#else
    if (fallback_buffer) {
        munmap(backing_base, backing_size);
    }
#endif
}

HostMemory::HostMemory(HostMemory&&) noexcept = default;

HostMemory& HostMemory::operator=(HostMemory&&) noexcept = default;

void HostMemory::Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perms, bool separate_heap) {
#if !(defined(__OPENORBIS__) || defined(__managarm__))
    ASSERT(virtual_offset % PageAlignment == 0);
    ASSERT(host_offset % PageAlignment == 0);
    ASSERT(length % PageAlignment == 0);
    ASSERT(virtual_offset + length <= virtual_size);
    ASSERT(host_offset + length <= backing_size);
    if (length == 0 || !virtual_base || !impl) {
        return;
    }
    impl->Map(virtual_offset + virtual_base_offset, host_offset, length, perms);
#endif
}

void HostMemory::Unmap(size_t virtual_offset, size_t length, bool separate_heap) {
#if !(defined(__OPENORBIS__) || defined(__managarm__))
    ASSERT(virtual_offset % PageAlignment == 0);
    ASSERT(length % PageAlignment == 0);
    ASSERT(virtual_offset + length <= virtual_size);
    if (length == 0 || !virtual_base || !impl) {
        return;
    }
    impl->Unmap(virtual_offset + virtual_base_offset, length);
#endif
}

void HostMemory::Protect(size_t virtual_offset, size_t length, MemoryPermission perm) {
#if !(defined(__OPENORBIS__) || defined(__managarm__))
    ASSERT(virtual_offset % PageAlignment == 0);
    ASSERT(length % PageAlignment == 0);
    ASSERT(virtual_offset + length <= virtual_size);
    if (length == 0 || !virtual_base || !impl) {
        return;
    }
    const bool read = True(perm & MemoryPermission::Read);
    const bool write = True(perm & MemoryPermission::Write);
    const bool execute = True(perm & MemoryPermission::Execute);
    impl->Protect(virtual_offset + virtual_base_offset, length, read, write, execute);
#endif
}

void HostMemory::HintCold(size_t physical_offset, size_t length,
                          std::optional<size_t> virtual_offset) const {
    if (length == 0 || backing_base == nullptr) {
        return;
    }
    const auto hint = [length](u8* address) {
#ifdef _WIN32
        // Unlocking pages that are not locked takes them out of the working set.
        VirtualUnlock(address, length);
#elif defined(__linux__) && defined(MADV_PAGEOUT)
        madvise(address, length, MADV_PAGEOUT);
#else
        static_cast<void>(address);
#endif
    };
    hint(backing_base + physical_offset);
    if (virtual_offset && virtual_base != nullptr && *virtual_offset + length <= virtual_size) {
        hint(virtual_base + *virtual_offset);
    }
}

void HostMemory::ClearBackingRegion(size_t physical_offset, size_t length, u32 fill_value) {
    if (length == 0) {
        return;
    }
    MarkUsed(physical_offset, length);
    if (fill_value != 0) {
        std::memset(backing_base + physical_offset, fill_value, length);
        MarkDirty(physical_offset, length);
        return;
    }
    // Fresh backing memory is zero and the host gives it no RAM until it's touched: clear only
    // the granules that may hold data (title 1's heap made the host commit every page of it).
    // A granule cleared in full is zero again.
#ifdef __linux__
    // Handing a whole granule back to the kernel frees its RAM; a memset would commit it.
    static const bool punch = [] {
        const char* value = std::getenv("EDEN_PUNCH_HOLE");
        return value == nullptr || value[0] != '0';
    }();
#endif
    const size_t end = physical_offset + length;
    for (size_t offset = physical_offset; offset < end;) {
        const size_t index = offset >> GranuleShift;
        const size_t granule_begin = index << GranuleShift;
        const size_t granule_end = granule_begin + (size_t{1} << GranuleShift);
        const size_t clear_end = (std::min)(granule_end, end);
        if ((granules[index].load(std::memory_order_acquire) & GranuleDirty) != 0) {
            const bool whole = offset == granule_begin && clear_end == granule_end;
            bool cleared = false;
#ifdef __linux__
            cleared = whole && punch && impl && impl->PunchHole(granule_begin, granule_end - granule_begin);
#endif
            if (!cleared) {
                std::memset(backing_base + offset, 0, clear_end - offset);
            }
            if (whole) {
                granules[index].fetch_and(static_cast<u8>(~GranuleDirty), std::memory_order_release);
            }
        }
        offset = clear_end;
    }
}

void HostMemory::MarkUsed(size_t physical_offset, size_t length) {
    if (length == 0) {
        return;
    }
    const size_t last = (physical_offset + length - 1) >> GranuleShift;
    // Windows, lazy commit: granules used for the first time are committed, in runs.
    size_t run_begin = SIZE_MAX;
    for (size_t index = physical_offset >> GranuleShift; index <= last; ++index) {
        bool fresh = false;
        if ((granules[index].load(std::memory_order_relaxed) & GranuleUsed) == 0) {
            fresh = (granules[index].fetch_or(GranuleUsed, std::memory_order_acq_rel) & GranuleUsed) == 0;
        }
        if (!lazy_commit) {
            continue;
        }
        if (fresh && run_begin == SIZE_MAX) {
            run_begin = index;
        } else if (!fresh && run_begin != SIZE_MAX) {
            CommitGranules(run_begin, index);
            run_begin = SIZE_MAX;
        }
    }
    if (run_begin != SIZE_MAX) {
        CommitGranules(run_begin, last + 1);
    }
}

void HostMemory::CommitGranules(size_t first, size_t end) {
#ifdef _WIN32
    const size_t offset = first << GranuleShift;
    const size_t length = (std::min)(end << GranuleShift, backing_size) - offset;
    if (!CommitRange(backing_base + offset, length)) {
        return;
    }
    const size_t before = lazy_committed_bytes.fetch_add(length, std::memory_order_relaxed);
    if ((before >> 30) != ((before + length) >> 30)) {
        LOG_INFO(HW_Memory, "Backing memory committed: {} MiB of {} MiB", (before + length) >> 20,
                 backing_size >> 20);
    }
#else
    static_cast<void>(first);
    static_cast<void>(end);
#endif
}

void HostMemory::MarkDirty(size_t physical_offset, size_t length) {
    if (length == 0) {
        return;
    }
    const size_t last = (physical_offset + length - 1) >> GranuleShift;
    for (size_t index = physical_offset >> GranuleShift; index <= last; ++index) {
        granules[index].fetch_or(GranuleDirty, std::memory_order_release);
    }
}

void HostMemory::ForgetContents() {
    for (size_t index = 0; index < num_granules; ++index) {
        if ((granules[index].load(std::memory_order_relaxed) & GranuleUsed) != 0) {
            granules[index].fetch_or(GranuleDirty, std::memory_order_release);
        }
    }
}

void HostMemory::EnableDirectMappedAddress() {
#if !(defined(__OPENORBIS__) || defined(__managarm__))
    if (impl) {
        impl->EnableDirectMappedAddress();
        virtual_size += reinterpret_cast<uintptr_t>(virtual_base);
    }
#endif
}

} // namespace Common
