/*  Copyright (C) 2026 qbspserver

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    See file, 'COPYING', for details.
*/

#include <common/sampler.hh>

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

namespace sampler
{
#ifdef _WIN32
// Each sample is a call stack: [depth, ip, return address, ...]. The buffer is preallocated: nothing
// may be allocated while a thread is stopped (it could hold the heap's lock).
constexpr size_t max_frames = 32;
constexpr size_t record_size = max_frames + 1;
static std::atomic<bool> running = false;
static std::thread thread;
static std::vector<uint64_t> samples;
static std::atomic<size_t> count = 0;
static std::atomic<size_t> lost = 0;

// Walks the stack of a stopped thread with the unwind data of its modules (no allocation). A bad
// stack (a thread stopped in the middle of something odd) ends the walk instead of crashing.
static size_t unwind(CONTEXT ctx, uint64_t *frames)
{
    size_t depth = 0;
    __try {
        while (depth < max_frames && ctx.Rip) {
            frames[depth++] = ctx.Rip;
            DWORD64 image_base = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
            if (!function) {
                // a leaf function: the return address is on top of the stack
                ctx.Rip = *reinterpret_cast<DWORD64 *>(ctx.Rsp);
                ctx.Rsp += 8;
            } else {
                PVOID handler_data = nullptr;
                DWORD64 establisher_frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, function, &ctx, &handler_data,
                    &establisher_frame, nullptr);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return depth;
}

static void loop()
{
    timeBeginPeriod(1);
    const DWORD pid = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    std::unordered_map<DWORD, HANDLE> handles;
    std::vector<HANDLE> active;
    auto last_scan = GetTickCount64() - 1000;
    const size_t capacity = samples.size() / record_size;

    while (running) {
        // the thread list, now and then (this allocates: no thread is stopped meanwhile)
        if (GetTickCount64() - last_scan >= 100) {
            last_scan = GetTickCount64();
            active.clear();
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap != INVALID_HANDLE_VALUE) {
                THREADENTRY32 te{};
                te.dwSize = sizeof(te);
                for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) {
                        continue;
                    }
                    auto [it, inserted] = handles.try_emplace(te.th32ThreadID, nullptr);
                    if (inserted) {
                        it->second = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                            FALSE, te.th32ThreadID);
                    }
                    if (it->second) {
                        active.push_back(it->second);
                    }
                }
                CloseHandle(snap);
            }
        }

        for (HANDLE h : active) {
            if (SuspendThread(h) == (DWORD)-1) {
                continue;
            }
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (!GetThreadContext(h, &ctx)) {
                ResumeThread(h);
                continue;
            }
            const size_t i = count.fetch_add(1);
            if (i < capacity) {
                uint64_t *record = &samples[i * record_size];
                record[0] = unwind(ctx, record + 1);
            } else {
                lost++;
            }
            ResumeThread(h);
        }
        Sleep(1);
    }
    for (auto &[tid, h] : handles) {
        if (h) {
            CloseHandle(h);
        }
    }
    timeEndPeriod(1);
}

void start()
{
    if (running) {
        return;
    }
    samples.assign(size_t(400000) * record_size, 0); // 400k stacks (~100 MB)
    count = 0;
    lost = 0;
    running = true;
    thread = std::thread(loop);
}

struct symbol_t
{
    std::string function; // "[module]" without debug info
    std::string line; // "file:line", or empty
    bool ours; // in the program itself (not the system, the C runtime or TBB)
};

std::string stop_and_report(size_t top)
{
    if (!running) {
        return {};
    }
    running = false;
    thread.join();

    const size_t n = std::min<size_t>(count, samples.size() / record_size);
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(proc, nullptr, TRUE);

    char exe_path[MAX_PATH] = "";
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    const HMODULE exe = GetModuleHandleA(nullptr);

    std::unordered_map<uint64_t, symbol_t> symbols;
    auto symbol = [&](uint64_t ip) -> const symbol_t & {
        auto [it, inserted] = symbols.try_emplace(ip);
        if (!inserted) {
            return it->second;
        }
        symbol_t &s = it->second;
        HMODULE module = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(ip), &module);
        s.ours = module == exe;
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
        auto *info = reinterpret_cast<SYMBOL_INFO *>(buffer);
        info->SizeOfStruct = sizeof(SYMBOL_INFO);
        info->MaxNameLen = 511;
        DWORD64 displacement = 0;
        if (SymFromAddr(proc, ip, &displacement, info)) {
            s.function = info->Name;
        } else {
            char name[MAX_PATH] = "?";
            if (module) {
                GetModuleFileNameA(module, name, MAX_PATH);
            }
            std::string m = name;
            s.function = "[" + m.substr(m.find_last_of("\\/") + 1) + "]";
        }
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD line_displacement = 0;
        if (SymGetLineFromAddr64(proc, ip, &line_displacement, &line)) {
            std::string file = line.FileName;
            s.line = fmt::format("{}:{}", file.substr(file.find_last_of("\\/") + 1), line.LineNumber);
        }
        return s;
    };

    auto is_idle = [](const std::string &f) {
        // a thread waiting for work isn't working ("is_tbbmalloc_used": tbb12.dll's spin loop, mis-named)
        return f.find("Wait") != std::string::npos || f.find("DelayExecution") != std::string::npos ||
               f.find("is_tbbmalloc_used") != std::string::npos;
    };
    auto is_memory = [](const symbol_t &s) {
        // the heap, tbbmalloc, the C runtime's malloc / free and new / delete
        static const char *names[] = {"AllocateHeap", "FreeHeap", "ReAllocateHeap", "RtlpHeap", "RtlpLowFrag",
            "tbbmalloc", "scalable_", "malloc", "free", "operator new", "operator delete", "_aligned_", "calloc",
            "realloc", "[ntdll.dll]", "[ucrtbase.dll]"};
        for (auto *name : names) {
            if (s.function.find(name) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    // the caller to blame for an allocation: the first frame in the program that isn't the standard
    // library's or an allocator's own code
    auto is_plumbing = [](const std::string &f) {
        return f.starts_with("std::") || f.starts_with("operator ") || f.starts_with("tbb::") ||
               f.find("allocator") != std::string::npos || f.starts_with("_") || f.starts_with("polylib::winding_storage");
    };

    std::map<std::string, size_t> by_function, by_line, inclusive, memory_callers, memory_lines;
    size_t idle = 0, memory = 0;
    std::set<std::string> seen;
    for (size_t i = 0; i < n; i++) {
        const uint64_t *record = &samples[i * record_size];
        const size_t depth = record[0];
        if (!depth) {
            continue;
        }
        const symbol_t &leaf = symbol(record[1]);
        if (is_idle(leaf.function)) {
            idle++;
            continue;
        }
        by_function[leaf.function]++;
        if (!leaf.line.empty()) {
            by_line[leaf.line + " (" + leaf.function + ")"]++;
        }
        seen.clear();
        bool in_memory = false;
        for (size_t d = 0; d < depth; d++) {
            const symbol_t &s = symbol(record[1 + d]);
            seen.insert(s.function);
            if (d < 6 && is_memory(s)) {
                in_memory = true;
            }
        }
        for (auto &f : seen) {
            inclusive[f]++;
        }
        if (in_memory) {
            memory++;
            for (size_t d = 0; d < depth; d++) {
                const symbol_t &s = symbol(record[1 + d]);
                if (s.ours && !is_plumbing(s.function)) {
                    memory_callers[s.function]++;
                    memory_lines[(s.line.empty() ? std::string("?") : s.line) + " (" + s.function + ")"]++;
                    break;
                }
            }
        }
    }
    SymCleanup(proc);

    const size_t busy = n - idle;
    auto print_top = [&](const std::string &title, const std::map<std::string, size_t> &m, size_t lines) {
        std::vector<std::pair<size_t, std::string>> v;
        for (auto &[k, c] : m) {
            v.emplace_back(c, k);
        }
        std::sort(v.begin(), v.end(), std::greater<>());
        std::string out = fmt::format("---- {} ----\n", title);
        for (size_t i = 0; i < v.size() && i < lines; i++) {
            out += fmt::format("  {:6.2f}%  {:8}  {}\n", 100.0 * v[i].first / std::max<size_t>(busy, 1), v[i].first,
                v[i].second.substr(0, 160));
        }
        return out;
    };
    std::string out = fmt::format("\n==== SAMPLES: {} ({} lost), all threads, ~1 ms apart; {} busy ({:.0f}%), "
                                  "percentages of the busy ones ====\n",
        n, lost.load(), busy, 100.0 * busy / std::max<size_t>(n, 1));
    out += print_top("by function", by_function, top);
    out += print_top("by source line", by_line, top);
    out += print_top("inclusive (the function or what it calls)", inclusive, top);
    out += print_top(fmt::format("memory (allocating / freeing: {:.1f}% of busy), by the caller in the program",
                         100.0 * memory / std::max<size_t>(busy, 1)),
        memory_callers, top);
    out += print_top("memory, by the caller's line", memory_lines, top);
    return out;
}
#else
void start() { }
std::string stop_and_report(size_t) { return {}; }
#endif
} // namespace sampler
