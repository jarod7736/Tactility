#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace agents {

/** Largest response body request() accepts. A larger body makes request() return nullopt. */
constexpr size_t HTTP_MAX_BODY = 4096;

struct HttpResponse {
    int status;
    std::string body;
};

/**
 * Minimal plain-HTTP client over BSD sockets (lwIP on ESP-IDF, POSIX on the host).
 * One connection per call. Requests are sent as HTTP/1.0 with `Connection: close`, so the
 * server (uvicorn) never uses chunked transfer encoding: bodies are delimited by
 * Content-Length or by the connection closing. A chunked response is rejected.
 */
class HttpClient {
public:
    HttpClient(std::string host, uint16_t port, std::string token);

    /** Send one request and read the whole response. nullopt on a network error, a malformed or
     *  chunked response, or a body over HTTP_MAX_BODY bytes. A non-empty jsonBody is sent as
     *  application/json; non-GET requests always carry Content-Length. */
    std::optional<HttpResponse> request(std::string_view method, std::string_view path, std::string_view jsonBody = {});

    /** GET path and pass body bytes to onChunk as they arrive, until onChunk returns false, the
     *  socket closes, a receive times out (30 s), or abortStream() is called.
     *  Returns false if the connection failed or the status was not 200 (see lastStatus()). */
    bool openStream(std::string_view path, const std::function<bool(std::string_view chunk)>& onChunk);

    /** Unblock a running openStream() from another thread. */
    void abortStream();

    /** HTTP status of the last request()/openStream(), or 0 when no response was received. */
    int lastStatus() const { return lastStatus_; }

private:
    int connectSocket(int receiveTimeoutSeconds) const;
    bool sendRequest(int fd, std::string_view method, std::string_view path, std::string_view jsonBody) const;

    std::string host_;
    uint16_t port_;
    std::string token_;
    int lastStatus_ = 0;

    std::mutex streamMutex_;
    int streamFd_ = -1;
    bool streamAborted_ = false;
};

/** Quote and escape text as a JSON string literal. UTF-8 passes through unchanged. */
std::string jsonString(std::string_view text);

} // namespace agents
