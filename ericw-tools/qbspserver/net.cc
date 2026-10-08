/*  qbspserver: local TCP connection between qbspserver and qbspreceive.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    See file, 'COPYING', for details.
*/

#include <qbspserver/net.hh>

#include <charconv>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using native_socket = SOCKET;
static void close_native(native_socket s)
{
    closesocket(s);
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using native_socket = int;
static void close_native(native_socket s)
{
    ::close(s);
}
#endif

// writing to a closed socket must fail with an error, not kill the process with SIGPIPE
#ifdef MSG_NOSIGNAL
constexpr int send_flags = MSG_NOSIGNAL;
#else
constexpr int send_flags = 0;
#endif

namespace net
{
bool startup()
{
#ifdef _WIN32
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return true;
#endif
}

socket::socket(uintptr_t handle)
    : handle_(handle)
{
}

socket::~socket()
{
    close();
}

socket::socket(socket &&other) noexcept
    : handle_(std::exchange(other.handle_, invalid_handle)),
      buffer_(std::move(other.buffer_))
{
}

socket &socket::operator=(socket &&other) noexcept
{
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, invalid_handle);
        buffer_ = std::move(other.buffer_);
    }
    return *this;
}

bool socket::valid() const
{
    return handle_ != invalid_handle;
}

uintptr_t socket::handle() const
{
    return handle_;
}

void socket::close()
{
    if (valid()) {
        close_native(static_cast<native_socket>(handle_));
        handle_ = invalid_handle;
    }
    buffer_.clear();
}

void socket::set_timeouts(int recv_ms, int send_ms)
{
    auto s = static_cast<native_socket>(handle_);
#ifdef _WIN32
    DWORD r = recv_ms, w = send_ms;
#else
    timeval r{recv_ms / 1000, (recv_ms % 1000) * 1000};
    timeval w{send_ms / 1000, (send_ms % 1000) * 1000};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&r), sizeof(r));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&w), sizeof(w));
}

bool socket::send_all(std::string_view data)
{
    auto s = static_cast<native_socket>(handle_);
    while (!data.empty()) {
        int sent = ::send(s, data.data(), static_cast<int>(data.size()), send_flags);
        if (sent <= 0) {
            return false;
        }
        data.remove_prefix(sent);
    }
    return true;
}

std::optional<std::string> socket::read_line()
{
    auto s = static_cast<native_socket>(handle_);
    for (;;) {
        if (size_t nl = buffer_.find('\n'); nl != std::string::npos) {
            std::string line = buffer_.substr(0, nl);
            buffer_.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return line;
        }

        char chunk[4096];
        int got = ::recv(s, chunk, sizeof(chunk), 0);
        if (got <= 0) {
            return std::nullopt;
        }
        buffer_.append(chunk, got);
    }
}

static sockaddr_in loopback(uint16_t port)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return addr;
}

socket listen_local(uint16_t port)
{
    native_socket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    socket result(static_cast<uintptr_t>(s));
    if (!result.valid()) {
        return result;
    }

    int on = 1;
#ifdef _WIN32
    // on Windows, SO_REUSEADDR would let a second server steal the port
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&on), sizeof(on));
#else
    // restart right away, without waiting for TIME_WAIT
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif

    sockaddr_in addr = loopback(port);
    if (::bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || ::listen(s, 8) != 0) {
        result.close();
    }
    return result;
}

socket accept(socket &listener)
{
    native_socket s = ::accept(static_cast<native_socket>(listener.handle()), nullptr, nullptr);
    return socket(static_cast<uintptr_t>(s));
}

socket connect_local(uint16_t port)
{
    native_socket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    socket result(static_cast<uintptr_t>(s));
    if (!result.valid()) {
        return result;
    }

    sockaddr_in addr = loopback(port);
    if (::connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        result.close();
    }
    return result;
}

std::optional<uint16_t> take_port_arg(std::vector<std::string> &args, std::string &error)
{
    uint16_t port = default_port;

    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] != "-port" && args[i] != "--port") {
            continue;
        }
        if (i + 1 >= args.size()) {
            error = "-port needs a value";
            return std::nullopt;
        }

        const std::string &value = args[i + 1];
        unsigned parsed = 0;
        auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (ec != std::errc() || end != value.data() + value.size() || parsed == 0 || parsed > 65535) {
            error = "invalid port: " + value;
            return std::nullopt;
        }

        port = static_cast<uint16_t>(parsed);
        args.erase(args.begin() + i, args.begin() + i + 2);
        i--;
    }

    return port;
}
} // namespace net
