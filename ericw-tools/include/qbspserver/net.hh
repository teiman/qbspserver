/*  qbspserver: local TCP connection between qbspserver and qbspreceive.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    See file, 'COPYING', for details.
*/

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace net
{
constexpr uint16_t default_port = 26600;

// exit codes of qbspreceive; the server sends 0, 1 or 3 in "done <code>"
enum exit_code : int
{
    EXIT_OK = 0, // compile succeeded
    EXIT_COMPILE_FAILED = 1, // errors in the map
    EXIT_NO_SERVER = 2, // could not reach the server, or the connection was lost
    EXIT_REJECTED = 3, // the server holds another map, or the request was malformed
};

// Winsock needs this once per process; does nothing elsewhere
bool startup();

// a TCP socket, closed on destruction
class socket
{
public:
    socket() = default;
    explicit socket(uintptr_t handle);
    ~socket();
    socket(socket &&other) noexcept;
    socket &operator=(socket &&other) noexcept;
    socket(const socket &) = delete;
    socket &operator=(const socket &) = delete;

    bool valid() const;
    uintptr_t handle() const;
    void close();

    // after this, a blocked recv / send fails instead of waiting forever (0 = no timeout)
    void set_timeouts(int recv_ms, int send_ms);

    // sends everything, or returns false if the connection is broken
    bool send_all(std::string_view data);

    // next line without its '\n' (and '\r'); nullopt on EOF, error or timeout
    std::optional<std::string> read_line();

private:
    uintptr_t handle_ = invalid_handle;
    std::string buffer_;

    static constexpr uintptr_t invalid_handle = ~uintptr_t(0);
};

// listens on 127.0.0.1:port only; invalid if the port is taken
socket listen_local(uint16_t port);
// waits for the next client
socket accept(socket &listener);
// connects to 127.0.0.1:port; invalid if nobody listens there
socket connect_local(uint16_t port);

// removes "-port N" (or "--port N") from args. Returns the port, the default if absent,
// or nullopt (with error set) if the value is not a valid port.
std::optional<uint16_t> take_port_arg(std::vector<std::string> &args, std::string &error);
} // namespace net
