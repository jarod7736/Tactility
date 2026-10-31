#include "agents/HttpClient.h"

#include <cerrno>
#include <charconv>
#include <cstdio>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace agents {

namespace {

constexpr int CONNECT_TIMEOUT_SECONDS = 5;
constexpr int CONNECT_SLICE_MS = 100;
constexpr int REQUEST_TIMEOUT_SECONDS = 10;
constexpr int STREAM_TIMEOUT_SECONDS = 30; // keepalives arrive every 15 s
constexpr size_t MAX_HEAD = 1024;
constexpr size_t READ_SIZE = 512;

#ifdef MSG_NOSIGNAL
constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#else
constexpr int SEND_FLAGS = 0;
#endif

struct ResponseHead {
    int status = 0;
    long contentLength = -1; // -1: delimited by connection close
    bool chunked = false;
    std::string leftover; // body bytes read together with the head
};

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}

/** recv that retries on EINTR. */
ssize_t receive(int fd, char* buffer, size_t size) {
    while (true) {
        ssize_t n = recv(fd, buffer, size, 0);
        if (n < 0 && errno == EINTR) continue;
        return n;
    }
}

bool sendAll(int fd, std::string_view data) {
    while (!data.empty()) {
        ssize_t n = send(fd, data.data(), data.size(), SEND_FLAGS);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}

bool parseHead(std::string_view text, ResponseHead& head) {
    // Status line: HTTP/1.x NNN Reason
    auto lineEnd = text.find("\r\n");
    auto statusLine = text.substr(0, lineEnd);
    if (!statusLine.starts_with("HTTP/") || statusLine.size() < 12) return false;
    auto code = statusLine.substr(9, 3);
    auto [end, error] = std::from_chars(code.data(), code.data() + code.size(), head.status);
    if (error != std::errc() || end != code.data() + code.size()) return false;

    text.remove_prefix(lineEnd == std::string_view::npos ? text.size() : lineEnd + 2);
    while (!text.empty()) {
        auto next = text.find("\r\n");
        auto line = text.substr(0, next);
        text.remove_prefix(next == std::string_view::npos ? text.size() : next + 2);
        auto colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        auto name = trim(line.substr(0, colon));
        auto value = trim(line.substr(colon + 1));
        if (equalsIgnoreCase(name, "content-length")) {
            long length = 0;
            auto [lengthEnd, lengthError] = std::from_chars(value.data(), value.data() + value.size(), length);
            if (lengthError != std::errc() || lengthEnd != value.data() + value.size() || length < 0) return false;
            head.contentLength = length;
        } else if (equalsIgnoreCase(name, "transfer-encoding") && !equalsIgnoreCase(value, "identity")) {
            head.chunked = true;
        }
    }
    return true;
}

/** Read up to and including the blank line after the headers. */
bool readHead(int fd, ResponseHead& head) {
    std::string buffer;
    char chunk[READ_SIZE];
    while (true) {
        auto end = buffer.find("\r\n\r\n");
        if (end != std::string::npos) {
            if (!parseHead(std::string_view(buffer).substr(0, end), head)) return false;
            head.leftover = buffer.substr(end + 4);
            return true;
        }
        if (buffer.size() > MAX_HEAD) return false;
        ssize_t n = receive(fd, chunk, sizeof(chunk));
        if (n <= 0) return false;
        buffer.append(chunk, static_cast<size_t>(n));
    }
}

void setTimeout(int fd, int option, int seconds) {
    timeval timeout {};
    timeout.tv_sec = seconds;
    setsockopt(fd, SOL_SOCKET, option, &timeout, sizeof(timeout));
}

} // namespace

HttpClient::HttpClient(std::string host, uint16_t port, std::string token) :
    host_(std::move(host)), port_(port), token_(std::move(token)) {}

int HttpClient::connectSocket(int receiveTimeoutSeconds) const {
    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    char port[8];
    snprintf(port, sizeof(port), "%u", static_cast<unsigned>(port_));
    if (getaddrinfo(host_.c_str(), port, &hints, &result) != 0 || result == nullptr) {
        return -1;
    }

    int fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(result);
        return -1;
    }

    // Non-blocking connect so the 5 s timeout is ours, not the stack's
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int connected = connect(fd, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);
    if (connected != 0) {
        if (errno != EINPROGRESS) {
            close(fd);
            return -1;
        }
        // Wait in short slices so abortStream() can cut a pending connect short
        int ready = 0;
        for (int slice = 0; slice < CONNECT_TIMEOUT_SECONDS * 1000 / CONNECT_SLICE_MS && ready == 0; slice++) {
            if (streamAborted_) break;
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(fd, &writable);
            timeval timeout {};
            timeout.tv_usec = CONNECT_SLICE_MS * 1000;
            ready = select(fd + 1, nullptr, &writable, nullptr, &timeout);
            if (ready < 0 && errno == EINTR) ready = 0;
        }
        int socketError = 0;
        socklen_t length = sizeof(socketError);
        if (ready <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &length) != 0 || socketError != 0) {
            close(fd);
            return -1;
        }
    }
    fcntl(fd, F_SETFL, flags);

    setTimeout(fd, SO_RCVTIMEO, receiveTimeoutSeconds);
    setTimeout(fd, SO_SNDTIMEO, CONNECT_TIMEOUT_SECONDS);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    return fd;
}

bool HttpClient::sendRequest(int fd, std::string_view method, std::string_view path, std::string_view jsonBody) const {
    // HTTP/1.0 so uvicorn answers without chunked encoding (a streamed body ends when the socket closes)
    std::string head;
    head.reserve(160 + path.size() + token_.size() + host_.size());
    head.append(method).append(" ").append(path).append(" HTTP/1.0\r\nHost: ").append(host_);
    head.append(":").append(std::to_string(port_));
    head.append("\r\nAuthorization: Bearer ").append(token_).append("\r\nConnection: close\r\n");
    if (!jsonBody.empty()) {
        head.append("Content-Type: application/json\r\n");
    }
    if (!jsonBody.empty() || method != "GET") {
        head.append("Content-Length: ").append(std::to_string(jsonBody.size())).append("\r\n");
    }
    head.append("\r\n");
    return sendAll(fd, head) && sendAll(fd, jsonBody);
}

std::optional<HttpResponse> HttpClient::request(std::string_view method, std::string_view path, std::string_view jsonBody) {
    lastStatus_ = 0;
    int fd = connectSocket(REQUEST_TIMEOUT_SECONDS);
    if (fd < 0) return std::nullopt;

    ResponseHead head;
    if (!sendRequest(fd, method, path, jsonBody) || !readHead(fd, head)) {
        close(fd);
        return std::nullopt;
    }
    lastStatus_ = head.status;
    if (head.chunked || head.contentLength > static_cast<long>(HTTP_MAX_BODY)) {
        close(fd);
        return std::nullopt;
    }

    std::string body = std::move(head.leftover);
    char chunk[READ_SIZE];
    bool ok = true;
    while (true) {
        if (head.contentLength >= 0 && body.size() >= static_cast<size_t>(head.contentLength)) {
            body.resize(static_cast<size_t>(head.contentLength));
            break;
        }
        if (body.size() > HTTP_MAX_BODY) {
            ok = false;
            break;
        }
        ssize_t n = receive(fd, chunk, sizeof(chunk));
        if (n == 0) {
            ok = head.contentLength < 0; // close-delimited body ends here; a short Content-Length body is an error
            break;
        }
        if (n < 0) {
            ok = false;
            break;
        }
        body.append(chunk, static_cast<size_t>(n));
    }
    close(fd);
    if (!ok || body.size() > HTTP_MAX_BODY) return std::nullopt;
    return HttpResponse { head.status, std::move(body) };
}

bool HttpClient::openStream(std::string_view path, const std::function<bool(std::string_view chunk)>& onChunk) {
    lastStatus_ = 0;
    int fd = connectSocket(STREAM_TIMEOUT_SECONDS);
    if (fd < 0) return false;
    {
        std::lock_guard lock(streamMutex_);
        if (streamAborted_) {
            close(fd);
            return false;
        }
        streamFd_ = fd;
    }

    ResponseHead head;
    bool opened = sendRequest(fd, "GET", path, {}) && readHead(fd, head);
    if (opened) {
        lastStatus_ = head.status;
        opened = head.status == 200 && !head.chunked;
    }
    if (opened) {
        bool more = head.leftover.empty() || onChunk(head.leftover);
        head.leftover = {};
        char chunk[READ_SIZE];
        while (more) {
            ssize_t n = receive(fd, chunk, sizeof(chunk));
            if (n <= 0) break;
            more = onChunk(std::string_view(chunk, static_cast<size_t>(n)));
        }
    }

    std::lock_guard lock(streamMutex_);
    streamFd_ = -1;
    close(fd);
    return opened;
}

void HttpClient::abortStream() {
    std::lock_guard lock(streamMutex_);
    streamAborted_ = true;
    if (streamFd_ >= 0) {
        shutdown(streamFd_, SHUT_RDWR);
    }
}

std::string jsonString(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (char c : text) {
        switch (c) {
            case '"': out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            case '\b': out.append("\\b"); break;
            case '\f': out.append("\\f"); break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char escaped[7];
                    snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out.append(escaped);
                } else {
                    out.push_back(c); // UTF-8 bytes pass through
                }
        }
    }
    out.push_back('"');
    return out;
}

} // namespace agents
