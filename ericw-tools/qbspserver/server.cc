/*  qbspserver: keeps one map loaded and compiles it when qbspreceive asks.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    See file, 'COPYING', for details.
*/

#include <qbspserver/net.hh>
#include <qbsp/qbsp.hh>
#include <qbsp/hullcache.hh>
#include <qbsp/map.hh>
#include <common/settings.hh>
#include <common/log.hh>
#include <fmt/format.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <optional>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// a client that connects but never sends its request must not block the server
constexpr int request_timeout_ms = 5000;
// a client that stops reading the log is dropped after this, so the server never waits on it
constexpr int send_timeout_ms = 10000;

/*
 * Sends the compile log to the client without slowing down the compile (see "Log streaming"
 * in qbspserver.md). The compile threads only append to a string; a sender thread drains it
 * to the socket in batches, as "log <line>" messages. If the client goes away the log is
 * dropped and the compile goes on.
 */
class log_stream
{
public:
    explicit log_stream(net::socket &sock)
        : sock_(sock),
          sender_([this] { run(); })
    {
    }

    ~log_stream() { finish(); }

    // called from any compile thread, through the logging print callback
    void push(const char *str)
    {
        if (broken_) {
            return;
        }
        {
            std::lock_guard lock(mutex_);
            pending_ += str;
        }
        wake_.notify_one();
    }

    // sends what is left and stops the sender thread
    void finish()
    {
        if (!sender_.joinable()) {
            return;
        }
        {
            std::lock_guard lock(mutex_);
            finished_ = true;
        }
        wake_.notify_one();
        sender_.join();
    }

    bool connected() const { return !broken_; }

private:
    void run()
    {
        std::string batch, partial, out;

        for (;;) {
            bool last;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return finished_ || !pending_.empty(); });
                batch.swap(pending_);
                last = finished_;
            }

            // the log arrives in arbitrary pieces; only whole lines are sent
            partial += batch;
            batch.clear();
            out.clear();

            size_t start = 0;
            for (size_t nl; (nl = partial.find('\n', start)) != std::string::npos; start = nl + 1) {
                append_line(out, std::string_view(partial).substr(start, nl - start));
            }
            partial.erase(0, start);

            if (last && !partial.empty()) {
                append_line(out, partial);
                partial.clear();
            }

            if (!out.empty() && !broken_ && !sock_.send_all(out)) {
                broken_ = true;
            }

            if (last) {
                return;
            }
        }
    }

    static void append_line(std::string &out, std::string_view line)
    {
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        out += "log ";
        out += line;
        out += '\n';
    }

    net::socket &sock_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::string pending_;
    bool finished_ = false;
    std::atomic<bool> broken_ = false;
    std::thread sender_;
};

// one full qbsp run, like the qbsp executable. Returns an exit code for "done".
// Ends with the per-phase time table ("==== BENCH ====", see log.cc), which tools/bench.py reads.
static int compile(const std::vector<std::string> &args)
{
    logging::bench_restart();

    try {
        InitQBSP(args);

        auto start = I_FloatTime();
        ProcessFile();
        auto end = I_FloatTime();

        logging::print("\n{:.3} seconds elapsed\n", (end - start));
        logging::print("{}", logging::bench_report());
        logging::close();
        return net::EXIT_OK;
    } catch (const settings::quit_after_help_exception &) {
        throw;
    } catch (const hullcache::limits_exceeded &) {
        // plain qbsp might have fit the format: build the clipping hulls like it does and retry
        // (see hullcache.hh). It stays that way for this server.
        logging::print("qbspserver: with the cell grid the output doesn't fit the format; compiling again "
                       "with the clipping hulls built like plain qbsp\n");
        logging::close();
        hullcache::hulls_like_qbsp();
        return compile(args);
    } catch (const std::exception &e) {
        logging::print("************ ERROR ************\n{}\n", e.what());
        logging::print("{}", logging::bench_report());
        logging::close();
        return net::EXIT_COMPILE_FAILED;
    }
}

/*
 * Nothing to do: the map file is byte for byte the one of the last successful compile, and the
 * wads it names and the .bsp written then are untouched, so the .bsp on disk is already the
 * result. (Never with misc_external_map, which reads other .map files.)
 */
struct file_stamp
{
    fs::path path;
    uintmax_t size = 0;
    fs::file_time_type time{};
    bool operator==(const file_stamp &) const = default;
};

static struct
{
    bool valid = false;
    std::string map_bytes;
    std::vector<file_stamp> files;
} last_output;

static std::string read_file(const fs::path &path)
{
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

static std::optional<file_stamp> stamp(const fs::path &path)
{
    std::error_code ec;
    file_stamp s{path, fs::file_size(path, ec), {}};
    if (ec) {
        return std::nullopt;
    }
    s.time = fs::last_write_time(path, ec);
    if (ec) {
        return std::nullopt;
    }
    return s;
}

// the files a successful compile read or wrote besides the .map: the .bsp, and the wads
static std::vector<file_stamp> output_stamps(const fs::path &map_path)
{
    std::vector<fs::path> paths{fs::absolute(fs::path(qbsp_options.bsp_path).replace_extension("bsp"))};
    for (const char *key : {"wad", "_wad"}) {
        std::string_view list = map.world_entity().epairs.get(key);
        while (!list.empty()) {
            size_t end = list.find(';');
            fs::path wad = list.substr(0, end);
            if (!wad.empty()) {
                paths.push_back(wad.is_absolute() ? wad : map_path.parent_path() / wad);
            }
            list.remove_prefix(end == std::string_view::npos ? list.size() : end + 1);
        }
    }
    std::vector<file_stamp> stamps;
    for (auto &p : paths) {
        if (auto s = stamp(p)) {
            stamps.push_back(*s);
        }
    }
    return stamps;
}

static void remember_output(const fs::path &map_path, std::string map_bytes, bool success)
{
    last_output.valid = success && map_bytes.find("misc_external_map") == std::string::npos;
    last_output.map_bytes = success ? std::move(map_bytes) : std::string();
    last_output.files = success ? output_stamps(map_path) : std::vector<file_stamp>{};
}

static bool output_up_to_date(const fs::path &map_path, const std::string &map_bytes)
{
    if (!last_output.valid || map_bytes != last_output.map_bytes) {
        return false;
    }
    for (auto &s : last_output.files) {
        if (stamp(s.path) != s) {
            return false;
        }
    }
    return true;
}

static bool same_file(const fs::path &a, const fs::path &b)
{
    std::error_code ec;
    if (fs::equivalent(a, b, ec)) {
        return true;
    }
    // the map may not exist (yet); compare the names
    return fs::absolute(a).lexically_normal() == fs::absolute(b).lexically_normal();
}

// reads one request from the client, compiles if it is for our map, and replies "done <code>"
static void serve(net::socket &client, const std::vector<std::string> &args, const fs::path &map_path)
{
    client.set_timeouts(request_timeout_ms, send_timeout_ms);

    auto request = client.read_line();
    if (!request) {
        return;
    }

    // "compile <map>": compile it; "forget <map>": drop cached data the last compile didn't use
    // (tools/bench.py, so every measured edit meets a server that only knows the base map)
    std::string_view command;
    for (std::string_view c : {"compile ", "forget "}) {
        if (request->starts_with(c)) {
            command = c;
        }
    }
    if (command.empty()) {
        logging::print("qbspserver: bad request: {}\n", *request);
        client.send_all("log ERROR: bad request, expected \"compile <map path>\"\n");
        client.send_all(fmt::format("done {}\n", static_cast<int>(net::EXIT_REJECTED)));
        return;
    }

    fs::path requested = request->substr(command.size());
    if (!same_file(requested, map_path)) {
        logging::print("qbspserver: rejected a request for {}\n", requested.string());
        client.send_all(fmt::format("log ERROR: this server holds {}, not {}\n", map_path.string(), requested.string()));
        client.send_all(fmt::format("done {}\n", static_cast<int>(net::EXIT_REJECTED)));
        return;
    }

    if (command == "forget ") {
        hullcache::forget_unused();
        client.send_all(fmt::format("done {}\n", static_cast<int>(net::EXIT_OK)));
        return;
    }

    logging::print("\nqbspserver: compile requested\n");

    std::string map_bytes = read_file(map_path);
    if (output_up_to_date(map_path, map_bytes)) {
        const std::string msg = fmt::format(
            "qbspserver: the map didn't change since the last compile: {} is up to date\n", last_output.files[0].path.string());
        logging::print("{}", msg);
        client.send_all("log " + msg);
        client.send_all(fmt::format("done {}\n", static_cast<int>(net::EXIT_OK)));
        return;
    }

    int code;
    {
        log_stream stream(client);
        logging::set_print_callback([&stream](logging::flag logflag, const char *str) {
            // progress bars are redrawn many times; they stay on the server console
            if (logflag == logging::flag::PERCENT || logflag == logging::flag::PROGRESS) {
                return;
            }
            stream.push(str);
        });

        code = compile(args);
        remember_output(map_path, std::move(map_bytes), code == net::EXIT_OK);

        logging::set_print_callback(nullptr);
        stream.finish();

        if (!stream.connected()) {
            logging::print("qbspserver: the client went away during the compile\n");
        }
    }

    client.send_all(fmt::format("done {}\n", code));
    logging::print("qbspserver: done ({}), waiting for qbspreceive\n", code);
}

static void print_usage()
{
    logging::print("usage: qbspserver [-port N] [-nohullcache] [qbsp options...] mapname[.map]\n"
                   "Compiles the map once, then compiles it again each time qbspreceive asks.\n"
                   "-nohullcache: rebuild every clipping hull on every compile (to compare)\n"
                   "Default port: {}\n",
        net::default_port);
}

static int qbspserver_main(int argc, const char **argv)
{
    std::vector<std::string> args(argv, argv + argc);

    std::string error;
    auto port = net::take_port_arg(args, error);
    if (!port) {
        logging::print("qbspserver: {}\n", error);
        return 1;
    }
    if (args.size() < 2) {
        print_usage();
        return 1;
    }

    // reuse unchanged clipping hulls between compiles, unless -nohullcache (to compare)
    bool use_hullcache = true;
    if (auto it = std::ranges::find(args, "-nohullcache"); it != args.end()) {
        args.erase(it);
        use_hullcache = false;
    }
    hullcache::set_enabled(use_hullcache);

    if (!net::startup()) {
        logging::print("qbspserver: could not start networking\n");
        return 1;
    }

    // listen before the first compile, so a taken port shows up right away
    net::socket listener = net::listen_local(*port);
    if (!listener.valid()) {
        logging::print("qbspserver: cannot listen on 127.0.0.1:{} (another qbspserver running?)\n", *port);
        return 1;
    }

    // first compile; also parses the qbsp options and finds the map
    int code = compile(args);

    fs::path map_path = qbsp_options.map_path;
    if (map_path.empty()) {
        return 1;
    }
    map_path = fs::absolute(map_path).lexically_normal();

    logging::print("\nqbspserver: first compile done ({})\n", code);
    logging::print("qbspserver: holding {}\n", map_path.string());
    logging::print("qbspserver: listening on 127.0.0.1:{}, waiting for qbspreceive\n", *port);

    for (;;) {
        net::socket client = net::accept(listener);
        if (!client.valid()) {
            continue;
        }
        serve(client, args, map_path);
    }
}

int main(int argc, const char **argv)
{
    logging::preinitialize();

    try {
        return qbspserver_main(argc, argv);
    } catch (const settings::quit_after_help_exception &) {
        return 1;
    } catch (const std::exception &e) {
        exit_on_exception(e);
    }
}
