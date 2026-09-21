// Module lifetime and thread-freeze helpers.
#ifndef OCCLUDE3D_UNLOAD_HPP_INCLUDED
#define OCCLUDE3D_UNLOAD_HPP_INCLUDED

#include <windows.h>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace occ
{
    inline bool readRaw(uintptr_t address, void* dest, size_t size)
    {
        if (!address || uint64_t(address) + size > 0x100000000ull) return false;
        __try { std::memcpy(dest, reinterpret_cast<void*>(address), size); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    inline bool bytesAre(uintptr_t at, const uint8_t* expected, size_t n)
    {
        uint8_t now[16] = {};
        return n <= sizeof now && readRaw(at, now, n) && std::memcmp(now, expected, n) == 0;
    }

    // This DLL's image span. False if it cannot be found.
    inline bool selfImage(const void* anyAddressInside, uintptr_t& lo, uintptr_t& hi)
    {
        HMODULE self = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                static_cast<LPCSTR>(anyAddressInside), &self) || !self)
            return false;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
        const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<uintptr_t>(self) + uintptr_t(dos->e_lfanew));
        lo = reinterpret_cast<uintptr_t>(self);
        hi = lo + nt->OptionalHeader.SizeOfImage;
        return true;
    }

    // Pin before the first client patch so hooks and return addresses always reach mapped code.
    inline bool pinSelf(const void* anyAddressInside)
    {
        HMODULE self = nullptr;
        return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                  static_cast<LPCSTR>(anyAddressInside), &self) != FALSE && self != nullptr;
    }

    struct CodeRange { uintptr_t lo = 0, hi = 0; };

    // Every other thread of this process, suspended until destruction.
    class ThreadFreeze
    {
    public:
        ThreadFreeze()
        {
            using NextThreadFn = LONG(NTAPI*)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);
            static const auto nextThread = reinterpret_cast<NextThreadFn>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtGetNextThread"));
            if (!nextThread) { complete_ = false; failStep_ = "NtGetNextThread missing"; return; }
            const DWORD self = GetCurrentThreadId();
            const ACCESS_MASK access = THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION;
            bool settled = false;
            for (int pass = 0; pass < kMaxPasses && complete_; pass++)
            {
                bool added = false;
                HANDLE current = nullptr;
                for (;;)
                {
                    HANDLE next = nullptr;
                    const LONG status = nextThread(GetCurrentProcess(), current, access, 0, 0, &next);
                    if (current) CloseHandle(current);
                    current = nullptr;
                    if (status == LONG(0x8000001A)) break;           // STATUS_NO_MORE_ENTRIES
                    if (status < 0 || !next) { complete_ = false; failStep_ = "walk"; failErr_ = DWORD(status); break; }
                    const DWORD id = GetThreadId(next);
                    if (id == self || holds(id)) { current = next; continue; }
                    if (count_ == kMaxThreads) { current = next; complete_ = false; failStep_ = "too many threads"; break; }
                    // An exited thread may still have a handle; skip it when SuspendThread refuses.
                    { DWORD code = 0; if (GetExitCodeThread(next, &code) && code != STILL_ACTIVE) { current = next; continue; } }
                    if (SuspendThread(next) == DWORD(-1)) { DWORD code = 0; if (GetExitCodeThread(next, &code) && code != STILL_ACTIVE) { current = next; continue; } current = next; complete_ = false; failStep_ = "SuspendThread"; failTid_ = id; failErr_ = GetLastError(); break; }
                    Thread& t = threads_[count_++];
                    t.id = id;
                    t.handle = next;
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_CONTROL;           // also waits for the suspension to take effect
                    t.ipKnown = GetThreadContext(next, &context) != FALSE;
                if (!t.ipKnown) { t.ctxErr = GetLastError(); ++unknown_; }
                    t.ip = t.ipKnown ? uintptr_t(context.Eip) : 0;
                    added = true;
                    if (!DuplicateHandle(GetCurrentProcess(), next, GetCurrentProcess(), &current, 0, FALSE, DUPLICATE_SAME_ACCESS)) { complete_ = false; failStep_ = "DuplicateHandle"; failErr_ = GetLastError(); break; }
                }
                if (current) CloseHandle(current);
                if (complete_ && !added) { settled = true; break; }
            }
            if (!settled) { complete_ = false; if (!failStep_) failStep_ = "not settled"; }
        }
        ~ThreadFreeze()
        {
            for (size_t i = 0; i < count_; i++) { ResumeThread(threads_[i].handle); CloseHandle(threads_[i].handle); }
        }
        ThreadFreeze(const ThreadFreeze&) = delete;
        ThreadFreeze& operator=(const ThreadFreeze&) = delete;

        // True if any suspended thread not in `skip` has its instruction pointer in one of `ranges`, or might.
        bool anyInRanges(const CodeRange* ranges, size_t n, const DWORD* skip = nullptr, size_t skipCount = 0) const
        {
            if (!complete_) return true;
            for (size_t i = 0; i < count_; i++)
            {
                bool skipped = false;
                for (size_t k = 0; k < skipCount && !skipped; k++) skipped = skip[k] != 0 && skip[k] == threads_[i].id;
                if (skipped) continue;
                if (!threads_[i].ipKnown) return true;
                for (size_t r = 0; r < n; r++)
                    if (threads_[i].ip >= ranges[r].lo && threads_[i].ip < ranges[r].hi) return true;
            }
            return false;
        }
        bool complete() const { return complete_; }
        void describe(char* out, size_t size, const CodeRange* ranges, size_t n, const DWORD* skip = nullptr, size_t skipCount = 0) const
        {
            int len = _snprintf_s(out, size, _TRUNCATE, "freeze %s (%s tid %lu err %lu), %u threads, %u without a readable context; in range:",
                                  complete_ ? "complete" : "INCOMPLETE", failStep_ ? failStep_ : "-", static_cast<unsigned long>(failTid_),
                                  static_cast<unsigned long>(failErr_), unsigned(count_), unsigned(unknown_));
            for (size_t i = 0; i < count_ && len > 0 && size_t(len) < size; i++)
            {
                bool skipped = false;
                for (size_t k = 0; k < skipCount && !skipped; k++) skipped = skip[k] != 0 && skip[k] == threads_[i].id;
                if (skipped) continue;
                bool hit = !threads_[i].ipKnown;
                for (size_t r = 0; r < n && !hit; r++) hit = threads_[i].ip >= ranges[r].lo && threads_[i].ip < ranges[r].hi;
                if (!hit) continue;
                const int added = _snprintf_s(out + len, size - size_t(len), _TRUNCATE, " tid %lu ip %08X%s", static_cast<unsigned long>(threads_[i].id), unsigned(threads_[i].ip),
                                   threads_[i].ipKnown ? "" : " (context unreadable)");
                if (added < 0) break;   // truncated: stop, never move the cursor back
                len += added;
            }
        }

    private:
        static constexpr size_t kMaxThreads = 512;
        static constexpr int kMaxPasses = 8;
        struct Thread { DWORD id = 0; HANDLE handle = nullptr; uintptr_t ip = 0; bool ipKnown = false; DWORD ctxErr = 0; };
        bool holds(DWORD id) const
        {
            for (size_t i = 0; i < count_; i++) if (threads_[i].id == id) return true;
            return false;
        }
        Thread threads_[kMaxThreads];
        size_t count_ = 0;
        bool complete_ = true;
        const char* failStep_ = nullptr;
        DWORD failTid_ = 0, failErr_ = 0;
        size_t unknown_ = 0;
    };

    // Try up to attempts quiet freezes, 1 ms apart. act must not allocate
    // or take a lock that a suspended thread could hold.
    template <class Act>
    inline bool whenNoThreadIn(const CodeRange* ranges, size_t n, const DWORD* skip, size_t skipCount, int attempts, Act&& act, char* why = nullptr, size_t whySize = 0)
    {
        for (int i = 0; i < attempts; i++)
        {
            {
                ThreadFreeze freeze;
                if (!freeze.anyInRanges(ranges, n, skip, skipCount)) { act(); return true; }
                if (why && whySize && i == attempts - 1) freeze.describe(why, whySize, ranges, n, skip, skipCount);   // the last try: why it was busy
            }
            Sleep(1);
        }
        return false;
    }
}

#endif
