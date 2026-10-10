/*  Copyright (C) 2000-2001  Kevin Shanahan

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

    See file, 'COPYING', for details.
*/

/*
 * common/log.c
 */

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <fmt/ostream.h>
#include <fmt/chrono.h>
#include <fmt/color.h>
#include <string>
#include <thread>
#include <algorithm>
#include <tuple>
#include <vector>
#include <cstdlib>

#include <common/log.hh>
#include <common/settings.hh>
#include <common/cmdlib.hh>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h> // for OutputDebugStringA
#include <io.h> // for _isatty

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#else // assume __unix__ is defined
#include <unistd.h> // for isatty
#endif // ifdef _WIN32

static std::ofstream logfile;

namespace logging
{
bitflags<flag> mask = bitflags<flag>(flag::ALL) & ~bitflags<flag>(flag::VERBOSE);
bool enable_color_codes = false;
bool break_on_error = true;

// Benchmark support, enabled by environment variables:
//   ERICW_BENCH=1       print a per-phase timing summary and a "BENCH ..." line on exit
//   ERICW_TIMELIMIT=N   (implies ERICW_BENCH) abort after N seconds and report how far it got
namespace bench
{
using clock = std::chrono::steady_clock;

static bool enabled = false;
static clock::time_point start_time;
static std::mutex phase_mutex;
struct phase_t
{
    std::string name;
    clock::time_point start;
    double cpu_start; // process CPU time (all threads), seconds
};
static std::vector<phase_t> phases;
static std::atomic<uint64_t> cur_count = 0, cur_max = 0, total_units = 0;
static std::atomic<bool> reported = false;
static std::string group; // prefix for phase names, see bench_set_group()

// CPU time used by the process so far (all threads), in seconds: a phase's CPU time over its wall
// time is how many cores it kept busy on average
static double process_cpu_seconds()
{
#ifdef _WIN32
    FILETIME creation, exit, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        return 0.0;
    }
    auto to_seconds = [](const FILETIME &t) {
        return ((static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime) * 1e-7; // 100 ns units
    };
    return to_seconds(kernel) + to_seconds(user);
#else
    timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

static double seconds_since(clock::time_point t)
{
    return std::chrono::duration<double>(clock::now() - t).count();
}

static std::string format_report(bool timed_out)
{
    const double elapsed = seconds_since(start_time);
    const uint64_t count = cur_count, max = cur_max, units = total_units;
    const bool determinate = max != 0 && max != indeterminate;
    const double pct = determinate ? 100.0 * static_cast<double>(std::min(count, max)) / max : 0.0;

    std::lock_guard lock(phase_mutex);
    const std::string name = phases.empty() ? "(start)" : phases.back().name;

    std::string out = fmt::format("\n==== BENCH ({}) ====\n", timed_out ? "timelimit" : "finished");
    // aggregate by name (qbsp repeats phases per entity), in order of first appearance
    std::vector<std::tuple<std::string, double, size_t, double>> totals;
    const double cpu_now = process_cpu_seconds();
    for (size_t i = 0; i < phases.size(); i++) {
        auto end = (i + 1 < phases.size()) ? phases[i + 1].start : clock::now();
        double secs = std::chrono::duration<double>(end - phases[i].start).count();
        double cpu = ((i + 1 < phases.size()) ? phases[i + 1].cpu_start : cpu_now) - phases[i].cpu_start;
        auto it = std::find_if(
            totals.begin(), totals.end(), [&](const auto &t) { return std::get<0>(t) == phases[i].name; });
        if (it == totals.end()) {
            totals.emplace_back(phases[i].name, secs, 1, cpu);
        } else {
            std::get<1>(*it) += secs;
            std::get<2>(*it)++;
            std::get<3>(*it) += cpu;
        }
    }
    for (auto &[phase_name, secs, times, cpu] : totals) {
        // [cores]: CPU time / wall time, the average number of cores the phase kept busy
        const std::string cores = secs >= 0.001 ? fmt::format("  [{:.1f} cores]", cpu / secs) : "";
        if (times > 1) {
            out += fmt::format("  {:>9.3f}s  {} (x{}){}\n", secs, phase_name, times, cores);
        } else {
            out += fmt::format("  {:>9.3f}s  {}{}\n", secs, phase_name, cores);
        }
    }
    out += fmt::format("BENCH status={} elapsed={:.3f} phase={} name=\"{}\" count={} max={} pct={:.2f} units={}\n",
        timed_out ? "timeout" : "done", elapsed, phases.size(), name, count, determinate ? max : 0, pct, units);
    return out;
}

static void report(bool timed_out)
{
    if (reported.exchange(true)) {
        return;
    }

    const std::string out = format_report(timed_out);
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
    if (logfile.is_open()) {
        logfile << out;
        logfile.flush();
    }
}

static void on_header(const char *name)
{
    if (!enabled) {
        return;
    }
    std::lock_guard lock(phase_mutex);
    phases.push_back({group.empty() ? std::string(name) : group + ": " + name, clock::now(), process_cpu_seconds()});
    cur_count = 0;
    cur_max = 0;
}

static void on_percent(uint64_t count, uint64_t max)
{
    if (!enabled) {
        return;
    }
    if (count < max) {
        total_units++;
    }
    if (cur_max != max) {
        cur_max = max;
        cur_count = 0;
    }
    uint64_t prev = cur_count;
    while (prev < count && !cur_count.compare_exchange_weak(prev, count))
        ;
}

static void init()
{
    const char *limit_env = std::getenv("ERICW_TIMELIMIT");
    const char *bench_env = std::getenv("ERICW_BENCH");
    const double limit = limit_env ? std::atof(limit_env) : 0.0;

    enabled = limit > 0 || (bench_env && std::atoi(bench_env) != 0);
    if (!enabled) {
        return;
    }

    start_time = clock::now();
    std::atexit([] { report(false); });

    if (limit > 0) {
        std::thread([limit] {
            std::this_thread::sleep_until(start_time + std::chrono::duration<double>(limit));
            report(true);
            std::_Exit(2);
        }).detach();
    }
}
} // namespace bench

void bench_restart()
{
    std::lock_guard lock(bench::phase_mutex);
    bench::enabled = true;
    bench::start_time = bench::clock::now();
    // time before the first header (option parsing, filesystem / wad setup...)
    bench::phases.assign(1, {"(init)", bench::start_time, bench::process_cpu_seconds()});
    bench::group.clear();
    bench::cur_count = 0;
    bench::cur_max = 0;
    bench::total_units = 0;
}

std::string bench_report()
{
    return bench::format_report(false);
}

void bench_set_group(std::string group)
{
    std::lock_guard lock(bench::phase_mutex);
    // the time until the group's first header is its own phase, not the end of the previous one
    if (bench::enabled) {
        std::string name = group.empty() ? "(after " + bench::group + ")" : group + ": (start)";
        bench::phases.push_back({std::move(name), bench::clock::now(), bench::process_cpu_seconds()});
    }
    bench::group = std::move(group);
}

static bool is_terminal()
{
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

void preinitialize()
{
#ifdef _WIN32
    // enable processing of ANSI escape sequences on Windows
    HANDLE hOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleMode(hOutput, ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_WRAP_AT_EOL_OUTPUT);

    // Windows throttles processes it considers background work (EcoQoS: lower clocks, efficiency
    // cores on hybrid CPUs): a compiler started from a script or an editor, or qbspserver waiting
    // between compiles. Measured on an Intel Core 7 240H: 25% slower. Ask for full speed.
    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    throttling.StateMask = 0; // control it, and turn it off
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));
#endif

    enable_color_codes = is_terminal();

    bench::init();
}

void init(std::optional<fs::path> filename, const settings::common_settings &settings)
{
    if (!settings.log.value()) {
        return;
    }

    if (settings.logfile.is_changed()) {
        filename = settings.logfile.value();
    }

    if (!filename.has_value()) {
        return;
    }

    fs::path p = fs::absolute(filename.value());

    if (logfile) {
        logfile.close();
    }

    logfile.open(p, settings.logappend.value() ? std::ios_base::app : std::ios_base::trunc);

    if (logfile) {
        print(flag::PROGRESS, "logging to {} ({})\n", p.string(), settings.logappend.value() ? "append" : "truncate");
        fmt::print(logfile, "---- {} / ericw-tools {} ----\n", settings.program_name, ERICWTOOLS_VERSION);
    } else {
        print(flag::PROGRESS, "WARNING: can't log to {}\n", p.string());
    }
}

void close()
{
    if (logfile) {
        fmt::print(logfile, "\n\n");
        logfile.close();
    }
}

static std::mutex print_mutex;
static print_callback_t active_print_callback;

void set_print_callback(print_callback_t cb)
{
    active_print_callback = cb;
}

void print(flag logflag, const char *str)
{
    if (!(mask & logflag)) {
        return;
    }

    if (active_print_callback) {
        active_print_callback(logflag, str);
    }

    fmt::text_style style;

    if (enable_color_codes) {
        if (string_icontains(str, "error")) {
            style = fmt::fg(fmt::color::red);
        } else if (string_icontains(str, "warning")) {
            style = fmt::fg(fmt::terminal_color::yellow);
        } else if (bitflags<flag>(logflag) & flag::PERCENT) {
            style = fmt::fg(fmt::terminal_color::bright_black);
        } else if (bitflags<flag>(logflag) & flag::STAT) {
            style = fmt::fg(fmt::terminal_color::cyan);
        }
    }

    print_mutex.lock();

    if (logflag != flag::PERCENT) {
        // log file, if open
        if (logfile && logflag != flag::PROGRESS) {
            logfile << str;
            logfile.flush();
        }

#ifdef _WIN32
        // print to windows console.
        // if VS's Output window gets support for ANSI colors, we can change this to ansi_str.c_str()
        OutputDebugStringA(str);
#endif
    }

    if (enable_color_codes) {
        // stdout (assume the terminal can render ANSI colors)
        fmt::print(style, "{}", str);
    } else {
        std::cout << str;
    }

    // for TB, etc...
    fflush(stdout);

    print_mutex.unlock();
}

void vprint(flag logflag, fmt::string_view format, fmt::format_args args)
{
    // see https://fmt.dev/10.0.0/api.html#argument-lists

    print(logflag, fmt::vformat(format, args).c_str());
}

void print(const char *str)
{
    print(flag::DEFAULT, str);
}

void vprint(fmt::string_view format, fmt::format_args args)
{
    vprint(flag::DEFAULT, format, args);
}

static qtime_point start_time;
static bool is_timing = false;
static uint64_t last_count = -1;
static qtime_point last_indeterminate_time;
static std::atomic_bool locked = false;
static std::array<duration, 10> one_percent_times;
static size_t num_percent_times, percent_time_index;
static qtime_point last_percent_time;

static duration average_times_for_one_percent()
{
    duration pt{};

    for (size_t i = 0; i < num_percent_times; i++) {
        pt += one_percent_times[i];
    }

    pt /= num_percent_times;

    return pt;
}

static void register_average_time(duration dt)
{
    one_percent_times[percent_time_index] = dt;
    percent_time_index = (percent_time_index + 1) % one_percent_times.size();

    if (num_percent_times < one_percent_times.size()) {
        num_percent_times++;
    }
}

void header(const char *name)
{
    bench::on_header(name);
    print(flag::PROGRESS, "---- {} ----\n", name);
}

void assert_(bool success, const char *expr, const char *file, int line)
{
    if (!success) {
        print("{}:{}: Q_assert({}) failed.\n", file, line, expr);
        // assert(0);
#ifdef _WIN32
        __debugbreak();
#endif
        exit(1);
    }
}

static percent_callback_t active_percent_callback;

void set_percent_callback(percent_callback_t cb)
{
    active_percent_callback = cb;
}

void percent(uint64_t count, uint64_t max, bool displayElapsed)
{
    bool expected = false;

    bench::on_percent(count, max);

    if (!(logging::mask & flag::CLOCK_ELAPSED)) {
        displayElapsed = false;
    }

    if (count == max) {
        while (!locked.compare_exchange_weak(expected, true))
            ; // wait until everybody else is done
    } else {
        if (!locked.compare_exchange_weak(expected, true)) {
            return; // somebody else is doing this already
        }
    }

    // we got the lock

    if (!is_timing) {
        start_time = I_FloatTime();
        is_timing = true;
        last_count = -1;
        last_indeterminate_time = {};
        num_percent_times = 0;
        percent_time_index = 0;
        last_percent_time = I_FloatTime();
    }

    if (count == max) {
        auto elapsed = I_FloatTime() - start_time;
        is_timing = false;
        if (displayElapsed) {
            if (max == indeterminate) {
                if (active_percent_callback) {
                    active_percent_callback(std::nullopt, elapsed);
                } else {
                    print(flag::PERCENT, "[done] time elapsed: {:%H:%M:%S}\n", elapsed);
                }
            } else {
                if (active_percent_callback) {
                    active_percent_callback(100u, elapsed);
                } else {
                    print(flag::PERCENT, "[100%] time elapsed: {:%H:%M:%S}\n", elapsed);
                }
            }
        }
        last_count = -1;
    } else {
        if (max != indeterminate) {
            uint32_t pct = static_cast<uint32_t>((static_cast<float>(count) / max) * 100);
            if (last_count != pct) {
                if (active_percent_callback) {
                    active_percent_callback(pct, std::nullopt);
                } else {
                    if (pct) {
                        // we shifted from some other percent value to a non-zero value;
                        // calculate the time it took to get a 1% change
                        uint64_t diff = pct - last_count; // should always be >= 1
                        duration dt = I_FloatTime() - last_percent_time;
                        dt /= diff;
                        register_average_time(dt);
                        last_percent_time = I_FloatTime();
                        print(flag::PERCENT, "[{:>3}%]  est: {:%H:%M:%S}\r", pct,
                            std::chrono::duration_cast<std::chrono::duration<long long>>(
                                average_times_for_one_percent() * (100 - pct)));
                    } else {
                        print(flag::PERCENT, "[{:>3}%]  ...\r", pct);
                    }
                }
                last_count = pct;
            }
        } else {
            auto t = I_FloatTime();

            if (t - last_indeterminate_time > std::chrono::milliseconds(100)) {
                constexpr const char *spinners[] = {".   ", " .  ", "  . ", "   ."};
                if (active_percent_callback) {
                    active_percent_callback(std::nullopt, std::nullopt);
                } else {
                    last_count = (last_count + 1) >= std::size(spinners) ? 0 : (last_count + 1);
                    print(flag::PERCENT, "[{}]\r", spinners[last_count]);
                }
                last_indeterminate_time = t;
            }
        }
    }

    // unlock for next call
    locked = false;
}

// percent_clock

percent_clock::percent_clock(uint64_t i_max)
    : max(i_max)
{
    if (max != 0) {
        percent(0, max, displayElapsed);
    }
}

void percent_clock::increase()
{
#ifdef _DEBUG
    if (count == max) {
        logging::print("ERROR TO FIX LATER: clock counter increased to end, but not finished yet\n");
    }
#endif

    percent(count++, max, displayElapsed);
}

void percent_clock::operator()()
{
    increase();
}

void percent_clock::operator++(int)
{
    increase();
}

void percent_clock::print()
{
    if (!ready) {
        return;
    }

    ready = false;

#ifdef _DEBUG
    if (max != indeterminate) {
        if (count != max) {
            logging::print("ERROR TO FIX LATER: clock counter ended too early\n");
        }
    }
#endif

    percent(max, max, displayElapsed);
}

percent_clock::~percent_clock()
{
    print();
}

// stat_tracker_t

stat_tracker_t::stat &stat_tracker_t::register_stat(const std::string &name, bool show_even_if_zero, bool is_warning)
{
    return stats.emplace_back(name, show_even_if_zero, is_warning);
}

size_t stat_tracker_t::number_of_digits(size_t n)
{
    return n ? ((size_t)log10(n) + 1) : 1;
}

size_t stat_tracker_t::number_of_digit_padding()
{
    size_t number_padding = 0;

    // calculate padding for number
    for (auto &stat : stats) {
        if (!stat.is_warning && (stat.show_even_if_zero || stat.count)) {
            number_padding = std::max(number_of_digits(stat.count.load()), number_padding);
        }
    }

    if (!number_padding) {
        return number_padding;
    }

    return number_padding + ((number_padding - 1) / 3);
}

void stat_tracker_t::print_stats()
{
    if (stats_printed) {
        return;
    }

    stats_printed = true;

    // add 8 char padding just to keep it away from the left side
    size_t number_padding = number_of_digit_padding() + 4;

    for (auto &stat : stats) {
        if (stat.show_even_if_zero || stat.count) {
            print(flag::STAT, "{}{:{}} {}\n", stat.is_warning ? "WARNING: " : "", fmt::group_digits(stat.count.load()),
                stat.is_warning ? 0 : number_padding, stat.name);
        }
    }
}

stat_tracker_t::~stat_tracker_t()
{
    print_stats();
}
}; // namespace logging

ericwtools_error::ericwtools_error(const char *what)
    : std::runtime_error(what)
{
}

[[noreturn]] void exit_on_exception(const std::exception &e)
{
    logging::print("************ ERROR ************\n{}\n", e.what());
    logging::close();
    exit(1);
}

/*
 * =================
 * Error
 * For abnormal program terminations
 * =================
 */
[[noreturn]] void Error(const char *error)
{
#ifdef _DEBUG
    if (logging::break_on_error) {
        __debugbreak();
    }
#endif
    throw ericwtools_error(error);
}

[[noreturn]] void VError(fmt::string_view format, fmt::format_args args)
{
    auto formatted = fmt::vformat(format, args);
    Error(formatted.c_str());
}
