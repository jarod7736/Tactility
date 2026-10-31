#include "agents/BridgeClient.h"

#include "agents/SseReader.h"

#include <Tactility/Tactility.h>
#include <Tactility/kernel/Kernel.h>
#include <Tactility/lvgl/LvglSync.h>
#include <Tactility/lvgl/Statusbar.h>
#include <Tactility/service/ServiceManifest.h>
#include <Tactility/service/ServiceRegistration.h>

#ifdef ESP_PLATFORM
#include <Tactility/service/wifi/Wifi.h>
#endif

#include <tactility/lvgl_icon_statusbar.h>

#include <cJSON.h>
#include <format>
#include <iterator>

namespace agents {

constexpr auto SERVICE_ID = "BridgeClient";
constexpr size_t WATCH_STACK_SIZE = 6144;
constexpr size_t SSE_MAX_LINE = 2048;
constexpr int MAX_PENDING_CARDS = 6;

namespace {

struct JsonDeleter {
    void operator()(cJSON* json) const { cJSON_Delete(json); }
};
using Json = std::unique_ptr<cJSON, JsonDeleter>;

Json parseJson(std::string_view text) {
    return Json(cJSON_ParseWithLength(text.data(), text.size()));
}

std::string stringField(const cJSON* object, const char* name) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : "";
}

std::optional<BridgeClient::SessionSummary> sessionFrom(const cJSON* object) {
    const cJSON* id = cJSON_GetObjectItemCaseSensitive(object, "id");
    if (!cJSON_IsObject(object) || !cJSON_IsString(id) || id->valuestring == nullptr) {
        return std::nullopt;
    }
    BridgeClient::SessionSummary session;
    session.id = id->valuestring;
    session.title = stringField(object, "title");
    const cJSON* updated = cJSON_GetObjectItemCaseSensitive(object, "updated");
    if (cJSON_IsNumber(updated)) {
        session.updated = static_cast<int64_t>(updated->valuedouble);
    }
    auto state = stringField(object, "state");
    session.state = state == "running" ? 'r' : state == "waiting" ? 'w' : 'i';
    return session;
}

bool networkUp() {
#ifdef ESP_PLATFORM
    return tt::service::wifi::getRadioState() == tt::service::wifi::RadioState::ConnectionActive;
#else
    return true;
#endif
}

} // namespace

// region Pure helpers

std::vector<BridgeClient::BackendInfo> parseBackends(std::string_view json) {
    std::vector<BridgeClient::BackendInfo> result;
    auto root = parseJson(json);
    if (!cJSON_IsArray(root.get())) return result;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root.get()) {
        if (!cJSON_IsObject(item)) continue;
        auto id = stringField(item, "id");
        if (id.empty()) continue;
        result.push_back(BridgeClient::BackendInfo {
            .id = std::move(id),
            .name = stringField(item, "name"),
            .ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "ok")) != 0,
            .reason = stringField(item, "reason"),
        });
    }
    return result;
}

std::vector<BridgeClient::SessionSummary> parseSessions(std::string_view json) {
    std::vector<BridgeClient::SessionSummary> result;
    auto root = parseJson(json);
    if (!cJSON_IsArray(root.get())) return result;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root.get()) {
        if (auto session = sessionFrom(item)) {
            result.push_back(std::move(*session));
        }
    }
    return result;
}

std::optional<BridgeClient::SessionSummary> parseSession(std::string_view json) {
    auto root = parseJson(json);
    return root ? sessionFrom(root.get()) : std::nullopt;
}

std::optional<BridgeClient::ModelList> parseModels(std::string_view json) {
    auto root = parseJson(json);
    const cJSON* current = cJSON_GetObjectItemCaseSensitive(root.get(), "current");
    const cJSON* choices = cJSON_GetObjectItemCaseSensitive(root.get(), "choices");
    if (!cJSON_IsObject(root.get()) || !cJSON_IsString(current) || current->valuestring == nullptr || !cJSON_IsArray(choices)) {
        return std::nullopt;
    }
    BridgeClient::ModelList models { .current = current->valuestring, .choices = {} };
    const cJSON* choice = nullptr;
    cJSON_ArrayForEach(choice, choices) {
        if (cJSON_IsString(choice) && choice->valuestring != nullptr) {
            models.choices.emplace_back(choice->valuestring);
        }
    }
    return models;
}

std::vector<Card> parseCards(std::string_view json) {
    std::vector<Card> result;
    auto root = parseJson(json);
    if (!cJSON_IsArray(root.get())) return result;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root.get()) {
        // Re-serialize each element so parseCard stays the single card parser (bodies are capped at 4 KB)
        char* text = cJSON_PrintUnformatted(item);
        if (text == nullptr) continue;
        if (auto card = parseCard(text)) {
            result.push_back(std::move(*card));
        }
        cJSON_free(text);
    }
    return result;
}

std::optional<Card> cardFromEvent(const SseEvent& event, int& afterSeq) {
    if (event.type == SseEvent::Type::Oversize) {
        Card card;
        card.kind = CardKind::Error;
        card.title = "card too large";
        card.local = true;
        return card;
    }
    auto card = parseCard(event.data);
    // seq 0 is the bridge's "Skipped" gap card: deliver it, but never resume from it.
    // A lower unresolved seq means the bridge restarted and numbering began again.
    if (card && card->seq > 0 && (card->seq > afterSeq || !card->resolved)) {
        afterSeq = card->seq;
    }
    return card;
}

int nextBackoffSeconds(int attempt) {
    constexpr int DELAYS[] = { 1, 2, 4, 8, 15 };
    constexpr int LAST = static_cast<int>(std::size(DELAYS)) - 1;
    return DELAYS[attempt < 0 ? 0 : attempt > LAST ? LAST : attempt];
}

std::string errorMessage(int status, std::string_view body) {
    if (status == 0) return "Bridge offline, retrying";
    if (status == 401) return "Check bridge.properties";
    if (status == 502) return "Backend unreachable";
    auto root = parseJson(body);
    if (cJSON_IsObject(root.get())) {
        auto error = stringField(root.get(), "error");
        if (error == "run_in_progress") return "Run in progress";
        if (error == "unknown_model") return "Unknown model";
        auto detail = stringField(root.get(), "detail");
        if (!detail.empty()) return detail;
    }
    return std::format("HTTP {}", status);
}

// endregion

// region BridgeClient

struct BridgeClient::Watch {
    Watch(const BridgeConfig& config, std::string path, int afterSeq, std::function<void(const Card&)> onCard) :
        client(config.host, config.port, config.token), path(std::move(path)), afterSeq(afterSeq), onCard(std::move(onCard)) {}

    HttpClient client;
    std::string path; // without the after= value
    int afterSeq;
    std::function<void(const Card&)> onCard;
    std::atomic<bool> stopping { false };
    std::atomic<int> pending { 0 }; // cards dispatched but not yet run on the main task
};

BridgeClient::~BridgeClient() {
    unwatch();
}

bool BridgeClient::onStart(tt::service::ServiceContext& context) {
    statusbarIcon_ = tt::lvgl::statusbar_icon_add(LVGL_ICON_STATUSBAR_CLOUD, false);
    auto config = loadBridgeConfig();
    if (config) {
        configure(std::move(*config));
    } else {
        {
            std::lock_guard lock(mutex_);
            configError_ = config.error();
        }
        setStatus(Status::NotConfigured);
    }
    // All statusbar slots can already be taken (STATUSBAR_ICON_LIMIT); then there is no icon to update.
    if (statusbarIcon_ >= 0) {
        auto current = status_.load();
        tt::lvgl::statusbar_icon_set_visibility(statusbarIcon_, current == Status::NotConfigured || current == Status::Unauthorized);
    }
    return true;
}

void BridgeClient::onStop(tt::service::ServiceContext& context) {
    unwatch();
    if (statusbarIcon_ >= 0) {
        tt::lvgl::statusbar_icon_remove(statusbarIcon_);
        statusbarIcon_ = -1;
    }
}

void BridgeClient::configure(BridgeConfig config) {
    {
        std::lock_guard lock(mutex_);
        config_ = std::move(config);
    }
    setStatus(Status::Ok);
}

void BridgeClient::setStatus(Status status) {
    auto previous = status_.exchange(status);
    if (previous != status && statusbarIcon_ >= 0) {
        tt::lvgl::statusbar_icon_set_visibility(statusbarIcon_, status == Status::NotConfigured || status == Status::Unauthorized);
    }
}

const char* BridgeClient::statusMessage() const {
    switch (status_.load()) {
        case Status::Ok:
            return "Connected";
        case Status::NotConfigured: {
            std::lock_guard lock(mutex_);
            return toMessage(configError_);
        }
        case Status::NoNetwork:
            return "No network";
        case Status::Offline:
            return "Bridge offline, retrying";
        case Status::Unauthorized:
            return "Check bridge.properties";
    }
    return "";
}

std::string BridgeClient::lastError() const {
    std::lock_guard lock(mutex_);
    return lastError_;
}

void BridgeClient::setLastError(std::string message) {
    std::lock_guard lock(mutex_);
    lastError_ = std::move(message);
}

std::optional<HttpResponse> BridgeClient::call(std::string_view method, const std::string& path, std::string_view body) {
    std::optional<BridgeConfig> config;
    {
        std::lock_guard lock(mutex_);
        config = config_;
    }
    if (!config) {
        setLastError(statusMessage());
        return std::nullopt;
    }
    if (!networkUp()) {
        setStatus(Status::NoNetwork);
        setLastError(statusMessage());
        return std::nullopt;
    }

    std::lock_guard requestLock(requestMutex_);
    HttpClient client(config->host, config->port, config->token);
    auto response = client.request(method, path, body);
    int status = response ? response->status : client.lastStatus();
    if (status == 0 || status >= 500) {
        setStatus(Status::Offline);
    } else if (status == 401) {
        setStatus(Status::Unauthorized);
    } else {
        setStatus(Status::Ok);
    }
    if (!response) {
        setLastError(status == 0 ? errorMessage(0, {}) : std::format("Bad response (HTTP {})", status));
    } else if (response->status != 200) {
        setLastError(errorMessage(response->status, response->body));
    }
    return response;
}

std::vector<BridgeClient::BackendInfo> BridgeClient::backends() {
    auto response = call("GET", "/v1/backends");
    return response && response->status == 200 ? parseBackends(response->body) : std::vector<BackendInfo>();
}

std::vector<BridgeClient::SessionSummary> BridgeClient::sessions(const std::string& backend) {
    auto response = call("GET", std::format("/v1/{}/sessions?limit=10", backend));
    return response && response->status == 200 ? parseSessions(response->body) : std::vector<SessionSummary>();
}

std::optional<BridgeClient::SessionSummary> BridgeClient::createSession(const std::string& backend) {
    auto response = call("POST", std::format("/v1/{}/sessions", backend), "{}");
    return response && response->status == 200 ? parseSession(response->body) : std::nullopt;
}

std::optional<BridgeClient::ModelList> BridgeClient::models(const std::string& backend) {
    auto response = call("GET", std::format("/v1/{}/models", backend));
    return response && response->status == 200 ? parseModels(response->body) : std::nullopt;
}

bool BridgeClient::setModel(const std::string& backend, const std::string& label) {
    auto response = call("PUT", std::format("/v1/{}/model", backend), std::format("{{\"label\":{}}}", jsonString(label)));
    return response && response->status == 200;
}

std::optional<std::vector<Card>> BridgeClient::older(const std::string& backend, const std::string& sid, int beforeSeq) {
    auto response = call("GET", std::format("/v1/{}/sessions/{}/cards?before={}&limit=10", backend, sid, beforeSeq));
    if (!response || response->status != 200) return std::nullopt;
    return parseCards(response->body);
}

bool BridgeClient::prompt(const std::string& backend, const std::string& sid, const std::string& text) {
    auto response = call("POST", std::format("/v1/{}/sessions/{}/prompt", backend, sid), std::format("{{\"text\":{}}}", jsonString(text)));
    return response && response->status == 200;
}

bool BridgeClient::respond(const std::string& backend, const std::string& sid, int seq, std::optional<std::string> choice, std::optional<std::string> answer) {
    std::string body = choice ? std::format("{{\"choice\":{}}}", jsonString(*choice))
        : answer ? std::format("{{\"answer\":{}}}", jsonString(*answer))
        : "{}";
    auto response = call("POST", std::format("/v1/{}/sessions/{}/cards/{}/respond", backend, sid, seq), body);
    return response && response->status == 200;
}

bool BridgeClient::stop(const std::string& backend, const std::string& sid) {
    auto response = call("POST", std::format("/v1/{}/sessions/{}/stop", backend, sid));
    return response && response->status == 200;
}

void BridgeClient::watch(const std::string& backend, const std::string& sid, int afterSeq, std::function<void(const Card&)> onCard) {
    unwatch();
    std::optional<BridgeConfig> config;
    {
        std::lock_guard lock(mutex_);
        config = config_;
    }
    if (!config) return;

    watch_ = std::make_shared<Watch>(
        *config,
        std::format("/v1/{}/sessions/{}/stream?after=", backend, sid),
        afterSeq,
        std::move(onCard)
    );
    auto watch = watch_;
    watchThread_ = std::make_unique<tt::Thread>("bridge_sse", WATCH_STACK_SIZE, [this, watch] { return runWatch(watch); });
    watchThread_->start();
}

void BridgeClient::unwatch() {
    if (!watchThread_) return;
    watch_->stopping = true;
    watch_->client.abortStream();
    watchThread_->join();
    watchThread_.reset();
    watch_.reset();
}

int32_t BridgeClient::runWatch(const std::shared_ptr<Watch>& watch) {
    auto deliver = [&watch](Card card) {
        // Backpressure: the main dispatcher queue is unbounded, so wait while it holds too many of our cards
        while (watch->pending > MAX_PENDING_CARDS && !watch->stopping) {
            tt::kernel::delayMillis(50);
        }
        if (watch->stopping) return;
        watch->pending++;
        tt::getMainDispatcher().dispatch([watch, card = std::move(card)] {
            // unwatch() runs with the LVGL lock held, so checking under the lock is race-free
            if (tt::lvgl::lock(portMAX_DELAY)) {
                if (!watch->stopping) {
                    watch->onCard(card);
                }
                tt::lvgl::unlock();
            }
            watch->pending--;
        });
    };

    int attempt = 0;
    while (!watch->stopping) {
        if (!networkUp()) {
            setStatus(Status::NoNetwork);
        } else {
            SseReader reader(SSE_MAX_LINE);
            bool receiving = false;
            watch->client.openStream(watch->path + std::to_string(watch->afterSeq), [&](std::string_view chunk) {
                if (!receiving) {
                    receiving = true;
                    attempt = 0;
                    setStatus(Status::Ok);
                }
                reader.feed(chunk, [&](SseEvent event) {
                    if (auto card = cardFromEvent(event, watch->afterSeq)) {
                        deliver(std::move(*card));
                    }
                });
                return !watch->stopping.load();
            });
            if (watch->stopping) break;

            int status = watch->client.lastStatus();
            if (status == 401) {
                setStatus(Status::Unauthorized);
                setLastError(errorMessage(401, {}));
                break; // no retry until watch() is called again
            }
            if (status == 404) {
                setLastError("Session not found");
                break;
            }
            if (status == 0 || status >= 500) {
                setStatus(Status::Offline);
            }
        }

        // Back off in short slices so unwatch() returns quickly
        int sliceCount = nextBackoffSeconds(attempt++) * 10;
        for (int i = 0; i < sliceCount && !watch->stopping; i++) {
            tt::kernel::delayMillis(100);
        }
    }
    return 0;
}

std::shared_ptr<BridgeClient> findBridgeClient() {
    return tt::service::findServiceById<BridgeClient>(SERVICE_ID);
}

extern const tt::service::ServiceManifest bridgeClientManifest = {
    .id = SERVICE_ID,
    .createService = tt::service::create<BridgeClient>
};

// endregion

} // namespace agents
