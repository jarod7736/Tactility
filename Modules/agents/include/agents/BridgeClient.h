#pragma once

#include "agents/BridgeConfig.h"
#include "agents/Card.h"
#include "agents/HttpClient.h"
#include "agents/SseReader.h"

#include <Tactility/Thread.h>
#include <Tactility/service/Service.h>
#include <Tactility/service/ServiceManifest.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agents {

/**
 * Tactility service (id "BridgeClient") that talks to cardputer-bridge.
 * Request methods block the caller (one request at a time) and must not be called with the LVGL lock
 * held for long; watch() runs the SSE stream on its own task.
 */
class BridgeClient final : public tt::service::Service {
public:
    struct BackendInfo {
        std::string id, name;
        bool ok = false;
        std::string reason;
    };

    struct SessionSummary {
        std::string id, title;
        int64_t updated = 0;
        char state = 'i'; // 'i' idle, 'r' running, 'w' waiting
    };

    struct ModelList {
        std::string current;
        std::vector<std::string> choices;
    };

    enum class Status { Ok, NotConfigured, NoNetwork, Offline, Unauthorized };

    ~BridgeClient() override;

    bool onStart(tt::service::ServiceContext& context) override;
    void onStop(tt::service::ServiceContext& context) override;

    /** Use this bridge (onStart does this from bridge.properties). Status becomes Ok. */
    void configure(BridgeConfig config);

    Status status() const { return status_; }
    const char* statusMessage() const;
    /** Message for the last failed request, e.g. "Run in progress". */
    std::string lastError() const;

    std::vector<BackendInfo> backends();
    std::vector<SessionSummary> sessions(const std::string& backend);
    std::optional<SessionSummary> createSession(const std::string& backend);
    std::optional<ModelList> models(const std::string& backend);
    bool setModel(const std::string& backend, const std::string& label);
    /** Up to 10 cards older than beforeSeq, oldest first. nullopt when the request fails (empty means none older). */
    std::optional<std::vector<Card>> older(const std::string& backend, const std::string& sid, int beforeSeq);
    /** False on 409 with lastError() "Run in progress". */
    bool prompt(const std::string& backend, const std::string& sid, const std::string& text);
    bool respond(const std::string& backend, const std::string& sid, int seq, std::optional<std::string> choice, std::optional<std::string> answer);
    bool stop(const std::string& backend, const std::string& sid);

    /** Stream cards after afterSeq on a task with a 6 KB stack, replacing any previous watch.
     *  Call watch() and unwatch() with the LVGL lock held (from any thread): onCard runs on the main
     *  dispatcher under the LVGL lock, and that is what guarantees it never runs after unwatch() returns.
     *  A line over 2 KB arrives as a local Error card "card too large" (Card::local, see Card.h).
     *  At most 7 cards wait in the dispatcher queue; the stream pauses until the main task catches up.
     *  Reconnects with backoff 1, 2, 4, 8, 15 s. 401 stops (Unauthorized); 404 stops with lastError()
     *  "Session not found"; no response or 5xx is Offline and keeps retrying.
     *  Known limit: backend and sid are put in the path without URL-encoding (bridge ids are URL-safe). */
    void watch(const std::string& backend, const std::string& sid, int afterSeq, std::function<void(const Card&)> onCard);
    void unwatch();

private:
    struct Watch;

    std::optional<HttpResponse> call(std::string_view method, const std::string& path, std::string_view body = {});
    void setStatus(Status status);
    void setLastError(std::string message);
    int32_t runWatch(const std::shared_ptr<Watch>& watch);

    mutable std::mutex mutex_; // guards config_, lastError_, configError_
    std::mutex requestMutex_; // one request at a time
    std::optional<BridgeConfig> config_;
    ConfigError configError_ = ConfigError::Missing;
    std::string lastError_;
    std::atomic<Status> status_ { Status::NotConfigured };
    int8_t statusbarIcon_ = -1;

    std::shared_ptr<Watch> watch_;
    std::unique_ptr<tt::Thread> watchThread_;
};

/** Manifest for the "BridgeClient" service (registered via tt::Configuration::services). */
extern const tt::service::ServiceManifest bridgeClientManifest;

/** The running BridgeClient service, or nullptr. */
std::shared_ptr<BridgeClient> findBridgeClient();

// Pure helpers (exposed for tests). Parsers return empty / nullopt on malformed JSON.
std::vector<BridgeClient::BackendInfo> parseBackends(std::string_view json);
std::vector<BridgeClient::SessionSummary> parseSessions(std::string_view json);
std::optional<BridgeClient::SessionSummary> parseSession(std::string_view json);
std::optional<BridgeClient::ModelList> parseModels(std::string_view json);
std::vector<Card> parseCards(std::string_view json);
/** Card for one SSE event, or nullopt for invalid JSON. Oversize gives a local Error card "card too large".
 *  afterSeq (the resume point) advances to a bridge card's seq when seq > 0 and either seq > afterSeq or the
 *  card is unresolved (a lower unresolved seq means the bridge restarted). Local and seq-0 cards never move it. */
std::optional<Card> cardFromEvent(const SseEvent& event, int& afterSeq);
/** Delay before reconnect attempt n (0-based): 1, 2, 4, 8, then 15 s. */
int nextBackoffSeconds(int attempt);
/** User-facing message for a failed response: 0 = no response. */
std::string errorMessage(int status, std::string_view body);

} // namespace agents
