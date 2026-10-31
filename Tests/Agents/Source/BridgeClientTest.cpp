#include <doctest.h>

#include <agents/BridgeClient.h>
#include <agents/HttpClient.h>

#include <Tactility/Tactility.h>
#include <Tactility/kernel/Kernel.h>

#include <arpa/inet.h>
#include <csignal>
#include <pthread.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace agents;

namespace {

/** Tiny HTTP server on 127.0.0.1. Each accepted connection gets the next canned response (the last one
 *  repeats). A response is a list of parts sent with a short pause between them, then the socket closes. */
class CannedServer {
public:
    explicit CannedServer(std::vector<std::vector<std::string>> responses) : responses(std::move(responses)) {
        listenFd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        socklen_t len = sizeof(addr);
        getsockname(listenFd, reinterpret_cast<sockaddr*>(&addr), &len);
        port = ntohs(addr.sin_port);
        listen(listenFd, 4);
        thread = std::thread([this] {
            // The FreeRTOS POSIX port drives its tick with signals; keep them off this thread so
            // its blocking socket calls are not interrupted.
            sigset_t all;
            sigfillset(&all);
            pthread_sigmask(SIG_BLOCK, &all, nullptr);
            run();
        });
    }

    ~CannedServer() {
        stopping = true;
        shutdown(listenFd, SHUT_RDWR);
        close(listenFd);
        thread.join();
    }

    uint16_t port = 0;
    std::atomic<int> accepted { 0 };

    std::vector<std::string> requests() {
        std::lock_guard lock(mutex);
        return received;
    }

private:
    void run() {
        while (!stopping) {
            int fd = accept(listenFd, nullptr, nullptr);
            if (fd < 0) return;
            int index = accepted++;
            timeval timeout { 2, 0 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            std::string request = readRequest(fd);
            {
                std::lock_guard lock(mutex);
                received.push_back(request);
            }
            const auto& parts = responses[std::min<size_t>(index, responses.size() - 1)];
            for (const auto& part : parts) {
                send(fd, part.data(), part.size(), 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
            // Wait (bounded) for the client to close first, so its last reads see every part.
            char buffer[64];
            while (recv(fd, buffer, sizeof(buffer), 0) > 0) {}
            close(fd);
        }
    }

    static std::string readRequest(int fd) {
        std::string request;
        char buffer[256];
        size_t headerEnd = std::string::npos;
        while (headerEnd == std::string::npos) {
            auto n = recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) return request;
            request.append(buffer, n);
            headerEnd = request.find("\r\n\r\n");
        }
        size_t length = 0;
        auto at = request.find("Content-Length: ");
        if (at != std::string::npos) length = std::stoul(request.substr(at + 16));
        while (request.size() < headerEnd + 4 + length) {
            auto n = recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            request.append(buffer, n);
        }
        return request;
    }

    std::vector<std::vector<std::string>> responses;
    int listenFd = -1;
    std::atomic<bool> stopping { false };
    std::thread thread;
    std::mutex mutex;
    std::vector<std::string> received;
};

std::string jsonResponse(int status, const std::string& body, const char* reason = "OK") {
    return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\ncontent-length: " + std::to_string(body.size()) +
        "\r\ncontent-type: application/json\r\nConnection: close\r\n\r\n" + body;
}

void configureLocal(BridgeClient& client, uint16_t port) {
    client.configure(BridgeConfig { .host = "127.0.0.1", .port = port, .token = "abc" });
}

} // namespace

// region HttpClient

TEST_CASE("request sends Authorization: Bearer and parses a 200 JSON body") {
    CannedServer server({ { jsonResponse(200, "{\"ok\":true}") } });
    HttpClient client("127.0.0.1", server.port, "abc");
    auto response = client.request("GET", "/v1/backends");
    REQUIRE(response.has_value());
    CHECK_EQ(response->status, 200);
    CHECK_EQ(response->body, "{\"ok\":true}");
    auto requests = server.requests();
    REQUIRE_EQ(requests.size(), 1);
    CHECK(requests[0].starts_with("GET /v1/backends HTTP/1.0\r\n"));
    CHECK(requests[0].find("\r\nAuthorization: Bearer abc\r\n") != std::string::npos);
    CHECK(requests[0].find("\r\nConnection: close\r\n") != std::string::npos);
}

TEST_CASE("request sends a JSON body with Content-Length") {
    CannedServer server({ { jsonResponse(200, "{}") } });
    HttpClient client("127.0.0.1", server.port, "abc");
    REQUIRE(client.request("POST", "/x", "{\"a\":1}").has_value());
    auto request = server.requests().at(0);
    CHECK(request.starts_with("POST /x HTTP/1.0\r\n"));
    CHECK(request.find("\r\nContent-Type: application/json\r\n") != std::string::npos);
    CHECK(request.find("\r\nContent-Length: 7\r\n") != std::string::npos);
    CHECK(request.ends_with("\r\n\r\n{\"a\":1}"));
}

TEST_CASE("request reads a close-delimited body split across reads") {
    CannedServer server({ { "HTTP/1.1 200 OK\r\nConnection: close\r\n", "\r\n[1,", "2]" } });
    HttpClient client("127.0.0.1", server.port, "abc");
    auto response = client.request("GET", "/x");
    REQUIRE(response.has_value());
    CHECK_EQ(response->body, "[1,2]");
}

TEST_CASE("request: body over 4096 bytes is nullopt") {
    std::string big(5000, 'x');
    SUBCASE("Content-Length") {
        CannedServer server({ { jsonResponse(200, big) } });
        HttpClient client("127.0.0.1", server.port, "abc");
        CHECK_FALSE(client.request("GET", "/x").has_value());
    }
    SUBCASE("close-delimited") {
        CannedServer server({ { "HTTP/1.1 200 OK\r\n\r\n" + big } });
        HttpClient client("127.0.0.1", server.port, "abc");
        CHECK_FALSE(client.request("GET", "/x").has_value());
    }
    SUBCASE("exactly 4096 is accepted") {
        CannedServer server({ { jsonResponse(200, std::string(4096, 'y')) } });
        HttpClient client("127.0.0.1", server.port, "abc");
        auto response = client.request("GET", "/x");
        REQUIRE(response.has_value());
        CHECK_EQ(response->body.size(), 4096);
    }
}

TEST_CASE("request rejects a chunked response") {
    CannedServer server({ { "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n0\r\n\r\n" } });
    HttpClient client("127.0.0.1", server.port, "abc");
    CHECK_FALSE(client.request("GET", "/x").has_value());
}

TEST_CASE("request to a closed port is nullopt with lastStatus 0") {
    uint16_t port;
    {
        CannedServer server({ { "" } });
        port = server.port;
    }
    HttpClient client("127.0.0.1", port, "abc");
    CHECK_FALSE(client.request("GET", "/x").has_value());
    CHECK_EQ(client.lastStatus(), 0);
}

TEST_CASE("jsonString escapes quotes, control characters and keeps UTF-8") {
    CHECK_EQ(jsonString("é\"\n"), "\"é\\\"\\n\"");
    CHECK_EQ(jsonString("a\\b\t\r\x01"), "\"a\\\\b\\t\\r\\u0001\"");
    CHECK_EQ(jsonString(""), "\"\"");
}

TEST_CASE("openStream delivers chunks until the callback returns false") {
    CannedServer server({ { "HTTP/1.1 200 OK\r\ncontent-type: text/event-stream\r\nConnection: close\r\n\r\n",
        "data: 1\n\n", "data: 2\n\n", "data: 3\n\n" } });
    HttpClient client("127.0.0.1", server.port, "abc");
    std::string seen;
    bool opened = client.openStream("/s", [&](std::string_view chunk) {
        seen.append(chunk);
        return seen.find("data: 2\n\n") == std::string::npos;
    });
    CHECK(opened);
    CHECK_EQ(client.lastStatus(), 200);
    CHECK(seen.find("data: 1\n\n") != std::string::npos);
    CHECK(seen.find("data: 3") == std::string::npos);
    CHECK(server.requests().at(0).starts_with("GET /s HTTP/1.0\r\n"));
}

TEST_CASE("openStream reads to connection close") {
    CannedServer server({ { "HTTP/1.1 200 OK\r\n\r\ndata: 1\n\n", "data: 2\n\n" } });
    HttpClient client("127.0.0.1", server.port, "abc");
    std::string seen;
    CHECK(client.openStream("/s", [&](std::string_view chunk) {
        seen.append(chunk);
        return true;
    }));
    CHECK_EQ(seen, "data: 1\n\ndata: 2\n\n");
}

TEST_CASE("openStream returns false on 401") {
    CannedServer server({ { jsonResponse(401, "{\"detail\":\"unauthorized\"}", "Unauthorized") } });
    HttpClient client("127.0.0.1", server.port, "abc");
    bool called = false;
    CHECK_FALSE(client.openStream("/s", [&](std::string_view) { return called = true; }));
    CHECK_FALSE(called);
    CHECK_EQ(client.lastStatus(), 401);
}

TEST_CASE("abortStream interrupts a pending connect") {
    // Non-routable address: the SYN gets no answer, so connect stays pending for the 5 s timeout.
    HttpClient client("10.255.255.1", 8765, "abc");
    std::atomic<long long> elapsedMs { -1 };
    std::thread streamer([&] {
        // Off the FreeRTOS tick signals, so select() is not cut short by EINTR
        sigset_t all;
        sigfillset(&all);
        pthread_sigmask(SIG_BLOCK, &all, nullptr);
        auto start = std::chrono::steady_clock::now();
        client.openStream("/s", [](std::string_view) { return true; });
        elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    client.abortStream();
    streamer.join();
    CHECK_GE(elapsedMs.load(), 150);
    CHECK_LT(elapsedMs.load(), 500);
}

// endregion

// region BridgeClient

TEST_CASE("BridgeClient: 401 on the stream sets Unauthorized and does not retry within 3 s") {
    CannedServer server({ { jsonResponse(401, "{\"detail\":\"unauthorized\"}", "Unauthorized") } });
    BridgeClient client;
    configureLocal(client, server.port);
    REQUIRE(client.status() == BridgeClient::Status::Ok);
    client.watch("hermes", "s1", 0, [](const Card&) {});
    for (int i = 0; i < 40 && client.status() != BridgeClient::Status::Unauthorized; i++) {
        tt::kernel::delayMillis(50);
    }
    CHECK(client.status() == BridgeClient::Status::Unauthorized);
    CHECK_EQ(std::string(client.statusMessage()), "Check bridge.properties");
    tt::kernel::delayMillis(3000);
    CHECK_EQ(server.accepted.load(), 1);
    CHECK(server.requests().at(0).starts_with("GET /v1/hermes/sessions/s1/stream?after=0 HTTP/1.0\r\n"));
    client.unwatch();
}

TEST_CASE("BridgeClient: 401 on a request sets Unauthorized") {
    CannedServer server({ { jsonResponse(401, "{\"detail\":\"unauthorized\"}", "Unauthorized") } });
    BridgeClient client;
    configureLocal(client, server.port);
    CHECK(client.backends().empty());
    CHECK(client.status() == BridgeClient::Status::Unauthorized);
}

TEST_CASE("BridgeClient: unconfigured client reports NotConfigured") {
    BridgeClient client;
    CHECK(client.status() == BridgeClient::Status::NotConfigured);
    CHECK_EQ(std::string(client.statusMessage()), "Bridge not configured");
    CHECK(client.backends().empty());
}

TEST_CASE("BridgeClient: models parses current and choices; setModel sends PUT label") {
    CannedServer server({
        { jsonResponse(200, "{\"current\":\"Coder (Lemonade)\",\"choices\":[\"Coder (Lemonade)\",\"Gufo\"]}") },
        { jsonResponse(200, "{\"current\":\"Gufo\"}") },
    });
    BridgeClient client;
    configureLocal(client, server.port);
    auto models = client.models("hermes");
    REQUIRE(models.has_value());
    CHECK_EQ(models->current, "Coder (Lemonade)");
    REQUIRE_EQ(models->choices.size(), 2);
    CHECK_EQ(models->choices[1], "Gufo");
    CHECK(client.setModel("hermes", "Gufo"));
    auto requests = server.requests();
    REQUIRE_EQ(requests.size(), 2);
    CHECK(requests[0].starts_with("GET /v1/hermes/models HTTP/1.0\r\n"));
    CHECK(requests[1].starts_with("PUT /v1/hermes/model HTTP/1.0\r\n"));
    CHECK(requests[1].ends_with("\r\n\r\n{\"label\":\"Gufo\"}"));
}

TEST_CASE("BridgeClient: setModel 404 unknown_model is false") {
    CannedServer server({ { jsonResponse(404, "{\"error\":\"unknown_model\"}", "Not Found") } });
    BridgeClient client;
    configureLocal(client, server.port);
    CHECK_FALSE(client.setModel("hermes", "Nope"));
    CHECK(client.status() == BridgeClient::Status::Ok);
}

TEST_CASE("BridgeClient: prompt 409 is false with Run in progress") {
    CannedServer server({ { jsonResponse(409, "{\"error\":\"run_in_progress\"}", "Conflict") } });
    BridgeClient client;
    configureLocal(client, server.port);
    CHECK_FALSE(client.prompt("opencode", "s1", "hé \"x\""));
    CHECK_EQ(client.lastError(), "Run in progress");
    auto request = server.requests().at(0);
    CHECK(request.starts_with("POST /v1/opencode/sessions/s1/prompt HTTP/1.0\r\n"));
    CHECK(request.ends_with("\r\n\r\n{\"text\":\"hé \\\"x\\\"\"}"));
}

TEST_CASE("BridgeClient: respond sends choice or answer; stop posts") {
    CannedServer server({ { jsonResponse(200, "{\"ok\":true}") } });
    BridgeClient client;
    configureLocal(client, server.port);
    CHECK(client.respond("hermes", "s1", 7, std::string("Y"), std::nullopt));
    CHECK(client.respond("hermes", "s1", 8, std::nullopt, std::string("blue")));
    CHECK(client.stop("hermes", "s1"));
    auto requests = server.requests();
    REQUIRE_EQ(requests.size(), 3);
    CHECK(requests[0].starts_with("POST /v1/hermes/sessions/s1/cards/7/respond HTTP/1.0\r\n"));
    CHECK(requests[0].ends_with("\r\n\r\n{\"choice\":\"Y\"}"));
    CHECK(requests[1].ends_with("\r\n\r\n{\"answer\":\"blue\"}"));
    CHECK(requests[2].starts_with("POST /v1/hermes/sessions/s1/stop HTTP/1.0\r\n"));
}

TEST_CASE("BridgeClient: respond 409 reports already resolved") {
    CannedServer server({ { jsonResponse(409, "{\"detail\":\"already resolved\"}", "Conflict") } });
    BridgeClient client;
    configureLocal(client, server.port);
    CHECK_FALSE(client.respond("hermes", "s1", 7, std::string("N"), std::nullopt));
    CHECK_EQ(client.lastError(), "already resolved");
}

TEST_CASE("BridgeClient: sessions, createSession and older hit the right paths") {
    CannedServer server({
        { jsonResponse(200, "[{\"id\":\"a\",\"title\":\"T\",\"updated\":5,\"state\":\"running\"}]") },
        { jsonResponse(200, "{\"id\":\"n\",\"title\":\"\",\"updated\":6,\"state\":\"idle\"}") },
        { jsonResponse(200, "[{\"seq\":3,\"kind\":\"text\"},{\"seq\":4,\"kind\":\"done\"}]") },
    });
    BridgeClient client;
    configureLocal(client, server.port);
    auto sessions = client.sessions("hermes");
    REQUIRE_EQ(sessions.size(), 1);
    CHECK_EQ(sessions[0].state, 'r');
    auto created = client.createSession("hermes");
    REQUIRE(created.has_value());
    CHECK_EQ(created->id, "n");
    auto cards = client.older("hermes", "a", 5);
    REQUIRE(cards.has_value());
    REQUIRE_EQ(cards->size(), 2);
    CHECK_EQ((*cards)[0].seq, 3);
    auto requests = server.requests();
    CHECK(requests[0].starts_with("GET /v1/hermes/sessions?limit=10 HTTP/1.0\r\n"));
    CHECK(requests[1].starts_with("POST /v1/hermes/sessions HTTP/1.0\r\n"));
    CHECK(requests[1].ends_with("\r\n\r\n{}"));
    CHECK(requests[2].starts_with("GET /v1/hermes/sessions/a/cards?before=5&limit=10 HTTP/1.0\r\n"));
}

TEST_CASE("BridgeClient: bridge down is Offline") {
    uint16_t port;
    {
        CannedServer server({ { "" } });
        port = server.port;
    }
    BridgeClient client;
    configureLocal(client, port);
    CHECK(client.sessions("hermes").empty());
    CHECK(client.status() == BridgeClient::Status::Offline);
    CHECK_EQ(std::string(client.statusMessage()), "Bridge offline, retrying");
}

TEST_CASE("BridgeClient: a 5xx response is Offline") {
    CannedServer server({ { jsonResponse(502, "{\"detail\":\"backend unreachable\"}", "Bad Gateway") } });
    BridgeClient client;
    configureLocal(client, server.port);
    CHECK(client.sessions("hermes").empty());
    CHECK(client.status() == BridgeClient::Status::Offline);
    CHECK_EQ(client.lastError(), "Backend unreachable");
}

TEST_CASE("BridgeClient: older distinguishes failure from no older cards") {
    CannedServer server({
        { jsonResponse(502, "{\"detail\":\"backend unreachable\"}", "Bad Gateway") },
        { jsonResponse(200, "[]") },
    });
    BridgeClient client;
    configureLocal(client, server.port);
    CHECK_FALSE(client.older("hermes", "a", 5).has_value());
    auto none = client.older("hermes", "a", 5);
    REQUIRE(none.has_value());
    CHECK(none->empty());
}

TEST_CASE("BridgeClient: 404 on the stream sets Session not found and does not retry") {
    CannedServer server({ { jsonResponse(404, "{\"detail\":\"not found\"}", "Not Found") } });
    BridgeClient client;
    configureLocal(client, server.port);
    client.watch("hermes", "gone", 0, [](const Card&) {});
    tt::kernel::delayMillis(2500); // past the first two backoff delays (1 s, 2 s would retry by 3 s)
    CHECK_EQ(server.accepted.load(), 1);
    CHECK_EQ(client.lastError(), "Session not found");
    client.unwatch();
}

TEST_CASE("BridgeClient: 5xx on the stream sets Offline and keeps retrying") {
    CannedServer server({ { jsonResponse(503, "{}", "Unavailable") } });
    BridgeClient client;
    configureLocal(client, server.port);
    client.watch("hermes", "s1", 0, [](const Card&) {});
    tt::kernel::delayMillis(1600); // first backoff is 1 s
    CHECK(client.status() == BridgeClient::Status::Offline);
    CHECK_GE(server.accepted.load(), 2);
    client.unwatch();
}

TEST_CASE("BridgeClient: unwatch interrupts a pending connect quickly") {
    BridgeClient client;
    // Non-routable address: the SYN gets no answer, so connect stays pending (or fails at once without a route)
    client.configure(BridgeConfig { .host = "10.255.255.1", .port = 8765, .token = "abc" });
    client.watch("hermes", "s1", 0, [](const Card&) {});
    tt::kernel::delayMillis(300);
    auto start = std::chrono::steady_clock::now();
    client.unwatch();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    CHECK_LT(elapsed.count(), 500);
}

TEST_CASE("BridgeClient: delivery backpressure holds at most 7 queued cards") {
    std::string events;
    for (int seq = 1; seq <= 12; seq++) {
        events += "data: {\"seq\":" + std::to_string(seq) + ",\"kind\":\"text\"}\n\n";
    }
    CannedServer server({ { "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n" + events } });
    BridgeClient client;
    configureLocal(client, server.port);
    auto& dispatcher = tt::getMainDispatcher();
    while (dispatcher.consume(0) > 0) {} // drain anything left by earlier tests
    client.watch("hermes", "s1", 0, [](const Card&) {});
    tt::kernel::delayMillis(400);
    CHECK_EQ(dispatcher.consume(0), 7); // the SSE task stops at 7 pending
    tt::kernel::delayMillis(400);
    CHECK_EQ(dispatcher.consume(0), 5); // the rest once the queue drained
    client.unwatch();
}

// endregion

// region Pure helpers

TEST_CASE("parseBackends reads id, name, ok and optional reason") {
    auto backends = parseBackends(
        "[{\"id\":\"hermes\",\"name\":\"Hermes\",\"ok\":true},{\"id\":\"opencode\",\"name\":\"OpenCode\",\"ok\":false,\"reason\":\"down\"}]");
    REQUIRE_EQ(backends.size(), 2);
    CHECK_EQ(backends[0].id, "hermes");
    CHECK(backends[0].ok);
    CHECK_EQ(backends[0].reason, "");
    CHECK_FALSE(backends[1].ok);
    CHECK_EQ(backends[1].reason, "down");
    CHECK(parseBackends("not json").empty());
}

TEST_CASE("parseSessions maps state to i/r/w") {
    auto sessions = parseSessions(
        "[{\"id\":\"1\",\"title\":\"a\",\"updated\":1700000000,\"state\":\"idle\"},"
        "{\"id\":\"2\",\"title\":\"b\",\"updated\":2,\"state\":\"running\"},"
        "{\"id\":\"3\",\"title\":\"c\",\"updated\":3,\"state\":\"waiting\"}]");
    REQUIRE_EQ(sessions.size(), 3);
    CHECK_EQ(sessions[0].state, 'i');
    CHECK_EQ(sessions[0].updated, 1700000000);
    CHECK_EQ(sessions[1].state, 'r');
    CHECK_EQ(sessions[2].state, 'w');
    CHECK(parseSessions("{}").empty());
}

TEST_CASE("parseModels rejects malformed input") {
    CHECK_FALSE(parseModels("[]").has_value());
    CHECK_FALSE(parseModels("{\"choices\":[]}").has_value());
}

TEST_CASE("parseCards keeps order and skips invalid cards") {
    auto cards = parseCards("[{\"seq\":1,\"kind\":\"user\"},{\"seq\":2,\"kind\":\"bogus\"},{\"seq\":3,\"kind\":\"step\"}]");
    REQUIRE_EQ(cards.size(), 2);
    CHECK_EQ(cards[0].seq, 1);
    CHECK_EQ(cards[1].seq, 3);
}

TEST_CASE("nextBackoffSeconds: 1, 2, 4, 8, then 15") {
    CHECK_EQ(nextBackoffSeconds(0), 1);
    CHECK_EQ(nextBackoffSeconds(1), 2);
    CHECK_EQ(nextBackoffSeconds(2), 4);
    CHECK_EQ(nextBackoffSeconds(3), 8);
    CHECK_EQ(nextBackoffSeconds(4), 15);
    CHECK_EQ(nextBackoffSeconds(100), 15);
}

TEST_CASE("cardFromEvent: Oversize is a local Error card and leaves afterSeq unchanged") {
    int after = 5;
    auto card = cardFromEvent(SseEvent { SseEvent::Type::Oversize, {} }, after);
    REQUIRE(card.has_value());
    CHECK(card->local);
    CHECK(card->kind == CardKind::Error);
    CHECK_EQ(card->title, "card too large");
    CHECK_EQ(after, 5);
}

TEST_CASE("cardFromEvent: afterSeq tracking") {
    int after = 5;
    auto data = [](const char* json) { return SseEvent { SseEvent::Type::Data, json }; };
    SUBCASE("newer card advances") {
        auto card = cardFromEvent(data("{\"seq\":6,\"kind\":\"text\"}"), after);
        REQUIRE(card.has_value());
        CHECK_FALSE(card->local);
        CHECK_EQ(after, 6);
    }
    SUBCASE("seq 0 gap card is delivered but does not move afterSeq") {
        auto card = cardFromEvent(data("{\"seq\":0,\"kind\":\"step\",\"title\":\"Skipped\"}"), after);
        REQUIRE(card.has_value());
        CHECK_EQ(card->seq, 0);
        CHECK_EQ(after, 5);
    }
    SUBCASE("resolved update for an older card does not move afterSeq") {
        REQUIRE(cardFromEvent(data("{\"seq\":3,\"kind\":\"approval\",\"resolved\":true}"), after).has_value());
        CHECK_EQ(after, 5);
    }
    SUBCASE("lower unresolved seq (bridge restart) resets afterSeq") {
        REQUIRE(cardFromEvent(data("{\"seq\":1,\"kind\":\"user\"}"), after).has_value());
        CHECK_EQ(after, 1);
    }
    SUBCASE("invalid JSON yields nothing") {
        CHECK_FALSE(cardFromEvent(data("{"), after).has_value());
        CHECK_EQ(after, 5);
    }
}

TEST_CASE("errorMessage") {
    CHECK_EQ(errorMessage(0, ""), "Bridge offline, retrying");
    CHECK_EQ(errorMessage(401, ""), "Check bridge.properties");
    CHECK_EQ(errorMessage(409, "{\"error\":\"run_in_progress\"}"), "Run in progress");
    CHECK_EQ(errorMessage(502, "{\"detail\":\"backend unreachable\"}"), "Backend unreachable");
    CHECK_EQ(errorMessage(404, "{\"detail\":\"no such card\"}"), "no such card");
    CHECK_EQ(errorMessage(500, "oops"), "HTTP 500");
}

// endregion
