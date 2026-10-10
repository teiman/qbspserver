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
static std::atomic<bool> running = false;
static std::thread thread;
static std::vector<uint64_t> samples; // instruction pointers, preallocated: no allocation while a thread is stopped
static std::atomic<size_t> count = 0;
static std::atomic<size_t> lost = 0;

static void loop()
{
    timeBeginPeriod(1);
    const DWORD pid = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    std::unordered_map<DWORD, HANDLE> handles;
    std::vector<HANDLE> active;
    auto last_scan = GetTickCount64() - 1000;

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
            ctx.ContextFlags = CONTEXT_CONTROL;
            const BOOL ok = GetThreadContext(h, &ctx);
            ResumeThread(h);
            if (!ok) {
                continue;
            }
            const size_t i = count.fetch_add(1);
            if (i < samples.size()) {
                samples[i] = ctx.Rip;
            } else {
                lost++;
            }
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
    samples.assign(size_t(32) << 20, 0); // 32M samples: ~30 min at 16 threads
    count = 0;
    lost = 0;
    running = true;
    thread = std::thread(loop);
}

std::string stop_and_report(size_t top)
{
    if (!running) {
        return {};
    }
    running = false;
    thread.join();

    const size_t n = std::min<size_t>(count, samples.size());
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(proc, nullptr, TRUE);

    // by function and by source line
    std::unordered_map<uint64_t, size_t> by_ip;
    for (size_t i = 0; i < n; i++) {
        by_ip[samples[i]]++;
    }
    std::map<std::string, size_t> by_function, by_line;
    size_t idle = 0;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
    for (auto &[ip, c] : by_ip) {
        auto *info = reinterpret_cast<SYMBOL_INFO *>(buffer);
        info->SizeOfStruct = sizeof(SYMBOL_INFO);
        info->MaxNameLen = 511;
        DWORD64 displacement = 0;
        std::string function;
        if (SymFromAddr(proc, ip, &displacement, info)) {
            function = info->Name;
        } else {
            char module[MAX_PATH] = "?";
            HMODULE hm = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCSTR>(ip), &hm)) {
                GetModuleFileNameA(hm, module, MAX_PATH);
            }
            std::string m = module;
            function = "[" + m.substr(m.find_last_of("\\/") + 1) + "]";
        }
        // a thread waiting for work isn't working: leave it out
        if (function.find("Wait") != std::string::npos || function.find("DelayExecution") != std::string::npos ||
            function.find("is_tbbmalloc_used") != std::string::npos) { // (tbb12.dll's spin loop, mis-named)
            idle += c;
            continue;
        }
        by_function[function] += c;

        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD line_displacement = 0;
        if (SymGetLineFromAddr64(proc, ip, &line_displacement, &line)) {
            std::string file = line.FileName;
            file = file.substr(file.find_last_of("\\/") + 1);
            by_line[fmt::format("{}:{} ({})", file, line.LineNumber, function)] += c;
        }
    }
    SymCleanup(proc);

    auto print_top = [&](const char *title, const std::map<std::string, size_t> &m) {
        std::vector<std::pair<size_t, std::string>> v;
        for (auto &[k, c] : m) {
            v.emplace_back(c, k);
        }
        std::sort(v.begin(), v.end(), std::greater<>());
        std::string out = fmt::format("---- {} ----\n", title);
        for (size_t i = 0; i < v.size() && i < top; i++) {
            out += fmt::format("  {:6.2f}%  {:8}  {}\n", 100.0 * v[i].first / std::max<size_t>(n - idle, 1),
                v[i].first, v[i].second.substr(0, 160));
        }
        return out;
    };
    std::string out = fmt::format("\n==== SAMPLES: {} ({} lost), all threads, ~1 ms apart; {} busy ({:.0f}%), "
                                  "percentages of the busy ones ====\n",
        n, lost.load(), n - idle, 100.0 * (n - idle) / std::max<size_t>(n, 1));
    out += print_top("by function", by_function);
    out += print_top("by source line", by_line);
    return out;
}
#else
void start() { }
std::string stop_and_report(size_t) { return {}; }
#endif
} // namespace sampler
