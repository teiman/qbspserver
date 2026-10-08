/*  qbspreceive: asks qbspserver to compile its map and waits for the result.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    See file, 'COPYING', for details.
*/

#include <qbspserver/net.hh>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int qbspreceive_main(int argc, const char **argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);

    std::string error;
    auto port = net::take_port_arg(args, error);
    if (!port) {
        std::fprintf(stderr, "qbspreceive: %s\n", error.c_str());
        return net::EXIT_REJECTED;
    }

    // -forget: instead of compiling, make the server drop cached data its last compile didn't
    // use (tools/bench.py does this between measured edits)
    std::string command = "compile ";
    if (auto it = std::find(args.begin(), args.end(), "-forget"); it != args.end()) {
        args.erase(it);
        command = "forget ";
    }

    if (args.size() != 1) {
        std::fprintf(stderr,
            "usage: qbspreceive [-port N] [-forget] mapname[.map]\n"
            "Asks qbspserver to compile the map and waits until it is done.\n"
            "-forget: don't compile; the server drops cached data its last compile didn't use\n"
            "Exit codes: 0 compiled, 1 compile failed, 2 no server, 3 rejected (server holds another map)\n"
            "Default port: %u\n",
            static_cast<unsigned>(net::default_port));
        return net::EXIT_REJECTED;
    }

    // same rule as qbsp: the extension is always .map
    fs::path map_path = args[0];
    map_path.replace_extension("map");
    map_path = fs::absolute(map_path).lexically_normal();

    if (!net::startup()) {
        std::fprintf(stderr, "qbspreceive: could not start networking\n");
        return net::EXIT_NO_SERVER;
    }

    net::socket server = net::connect_local(*port);
    if (!server.valid()) {
        std::fprintf(stderr, "qbspreceive: no qbspserver on 127.0.0.1:%u\n", static_cast<unsigned>(*port));
        return net::EXIT_NO_SERVER;
    }

    if (!server.send_all(command + map_path.string() + "\n")) {
        std::fprintf(stderr, "qbspreceive: lost the connection to qbspserver\n");
        return net::EXIT_NO_SERVER;
    }

    while (auto line = server.read_line()) {
        std::string_view msg = *line;

        if (msg.starts_with("log ")) {
            msg.remove_prefix(4);
            std::fwrite(msg.data(), 1, msg.size(), stdout);
            std::fputc('\n', stdout);
            std::fflush(stdout);
        } else if (msg.starts_with("done ")) {
            msg.remove_prefix(5);
            int code = 0;
            auto [end, ec] = std::from_chars(msg.data(), msg.data() + msg.size(), code);
            if (ec != std::errc() || end != msg.data() + msg.size()) {
                break;
            }
            return code;
        }
        // unknown messages are ignored, so the server can add new ones later
    }

    std::fprintf(stderr, "qbspreceive: lost the connection to qbspserver\n");
    return net::EXIT_NO_SERVER;
}

int main(int argc, const char **argv)
{
    return qbspreceive_main(argc, argv);
}
