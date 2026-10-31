#include "agents/AgentApps.h"

#include "AgentState.h"

#include <Tactility/app/App.h>

#include <algorithm>
#include <format>

namespace agents {

// region Pure helpers

std::string sessionListHeader(std::string_view backendName, std::string_view modelLabel) {
    if (modelLabel.empty()) return std::string(backendName);
    return std::format("{} • {}", backendName, modelLabel);
}

std::string sessionListMessage(bool statusOk, std::string_view statusMessage, const BridgeClient::BackendInfo* backend, size_t sessionCount) {
    if (!statusOk) return std::string(statusMessage);
    if (backend != nullptr && !backend->ok) {
        return backend->reason.empty() ? "Backend unavailable" : backend->reason;
    }
    if (sessionCount == 0) return "No sessions - press n";
    return {};
}

ListAction listKeyAction(uint32_t key) {
    switch (key) {
        case LV_KEY_PREV: return ListAction::Prev;
        case LV_KEY_NEXT: return ListAction::Next;
        case LV_KEY_ENTER: return ListAction::Open;
        case LV_KEY_ESC: return ListAction::Back;
        case 'n':
        case 'N': return ListAction::NewSession;
        case 'm':
        case 'M': return ListAction::PickModel;
        default: return ListAction::None;
    }
}

// endregion

static lv_color_t stateColor(char state) {
    switch (state) {
        case 'r': return lv_palette_main(LV_PALETTE_GREEN);
        case 'w': return lv_palette_main(LV_PALETTE_ORANGE);
        default: return lv_palette_main(LV_PALETTE_GREY);
    }
}

static void addRow(lv_obj_t* content, const std::string& text, lv_color_t dotColor, bool showDot) {
    auto* row = lv_obj_create(content);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, 1, 0);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);

    auto* dot = lv_label_create(row);
    lv_label_set_text(dot, "•");
    lv_obj_set_style_text_color(dot, dotColor, 0);
    if (!showDot) lv_obj_set_style_text_opa(dot, LV_OPA_TRANSP, 0);

    auto* label = lv_label_create(row);
    lv_label_set_text(label, text.c_str());
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(label, 1);
}

static void updateSelection(AgentState& state) {
    size_t selected = state.picking ? state.modelSelected : state.selected;
    uint32_t count = lv_obj_get_child_count(state.content);
    for (uint32_t i = 0; i < count; i++) {
        auto* row = lv_obj_get_child(state.content, static_cast<int32_t>(i));
        if (i == selected) {
            lv_obj_set_style_bg_color(row, lv_palette_main(LV_PALETTE_BLUE), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_scroll_to_view(row, LV_ANIM_OFF);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
    }
}

void refreshSessionList(AgentState& state) {
    // closeSession() switches mode before the async rebuild replaces the card widgets
    if (!isShown(state) || state.mode != AgentState::Mode::List || state.viewMode != AgentState::Mode::List) return;

    lv_obj_clean(state.content);
    bool rows = false;
    if (state.picking && state.models) {
        rows = true;
        lv_label_set_text(state.headerLeft, std::format("{} • pick model", state.name).c_str());
        lv_label_set_text(state.footer, "Enter set  Fn+` cancel");
        for (const auto& choice : state.models->choices) {
            addRow(state.content, choice, lv_palette_main(LV_PALETTE_GREEN), choice == state.models->current);
        }
    } else {
        std::string model = state.models ? state.models->current : std::string();
        lv_label_set_text(state.headerLeft, sessionListHeader(state.name, model).c_str());
        lv_label_set_text(state.footer, "[n] new [m] model  Fn+` exit");
        if (state.loading && state.sessions.empty()) {
            auto* label = lv_label_create(state.content);
            lv_label_set_text(label, "Loading...");
        } else if (!state.listMessage.empty()) {
            auto* label = lv_label_create(state.content);
            lv_obj_set_width(label, LV_PCT(100));
            lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
            lv_label_set_text(label, state.listMessage.c_str());
        } else {
            rows = true;
            for (const auto& session : state.sessions) {
                addRow(state.content, session.title.empty() ? session.id : session.title, stateColor(session.state), true);
            }
        }
    }
    lv_label_set_text(state.headerRight, state.loading ? "..." : "");
    if (rows) {
        updateSelection(state);
    }
}

void buildSessionList(const StatePtr& state) {
    lv_obj_set_flex_flow(state->content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(state->content, 0, 0);
    refreshSessionList(*state);
    if (!state->picking) {
        loadSessionList(state);
    }
}

void loadSessionList(const StatePtr& state) {
    if (state->client == nullptr) {
        state->listMessage = "Bridge service not running";
        refreshSessionList(*state);
        return;
    }
    if (state->loading) return;
    state->loading = true;
    refreshSessionList(*state);

    struct Loaded {
        std::vector<BridgeClient::BackendInfo> backends;
        std::vector<BridgeClient::SessionSummary> sessions;
        std::optional<BridgeClient::ModelList> models;
        bool statusOk = false;
        std::string statusMessage;
    };
    auto loaded = std::make_shared<Loaded>();
    auto client = state->client;
    auto backend = state->backend;
    runRequest(state, [client, backend, loaded] {
        loaded->backends = client->backends();
        bool backendOk = std::ranges::none_of(loaded->backends, [&](const auto& b) { return b.id == backend && !b.ok; });
        // Skip the other requests when the bridge or this backend is down: each could wait for a timeout.
        if (client->status() == BridgeClient::Status::Ok && backendOk) {
            loaded->models = client->models(backend);
            loaded->sessions = client->sessions(backend);
        }
        loaded->statusOk = client->status() == BridgeClient::Status::Ok;
        loaded->statusMessage = client->statusMessage();
    }, [state, loaded] {
        state->loading = false;
        state->backendInfo.reset();
        for (const auto& b : loaded->backends) {
            if (b.id == state->backend) state->backendInfo = b;
        }
        state->sessions = std::move(loaded->sessions);
        if (loaded->models) state->models = std::move(loaded->models);
        state->listMessage = sessionListMessage(
            loaded->statusOk,
            loaded->statusMessage,
            state->backendInfo ? &*state->backendInfo : nullptr,
            state->sessions.size()
        );
        if (state->selected >= state->sessions.size()) state->selected = 0;
        refreshSessionList(*state);
    });
}

static void moveSelection(size_t& selected, size_t count, bool next) {
    if (count == 0) return;
    if (next && selected + 1 < count) selected++;
    if (!next && selected > 0) selected--;
}

static void handlePickerKey(const StatePtr& state, ListAction action) {
    auto& choices = state->models->choices;
    switch (action) {
        case ListAction::Prev:
        case ListAction::Next:
            moveSelection(state->modelSelected, choices.size(), action == ListAction::Next);
            updateSelection(*state);
            break;
        case ListAction::Back:
            state->picking = false;
            refreshSessionList(*state);
            break;
        case ListAction::Open: {
            if (state->modelSelected >= choices.size() || state->loading) break;
            auto label = choices[state->modelSelected];
            auto client = state->client;
            auto backend = state->backend;
            auto ok = std::make_shared<bool>(false);
            auto error = std::make_shared<std::string>();
            state->loading = true;
            state->picking = false;
            refreshSessionList(*state);
            runRequest(state, [client, backend, label, ok, error] {
                *ok = client->setModel(backend, label);
                if (!*ok) *error = client->lastError();
            }, [state, label, ok, error] {
                state->loading = false;
                if (*ok) {
                    state->models->current = label;
                } else {
                    showToast(*state, error->empty() ? "Could not set model" : *error);
                    updateBanner(*state);
                }
                refreshSessionList(*state);
            });
            break;
        }
        default:
            break;
    }
}

void handleListKey(const StatePtr& state, uint32_t key) {
    auto action = listKeyAction(key);
    if (state->picking && state->models) {
        handlePickerKey(state, action);
        return;
    }
    bool rowsShown = state->listMessage.empty() && !state->sessions.empty();
    switch (action) {
        case ListAction::Prev:
        case ListAction::Next:
            if (rowsShown) {
                moveSelection(state->selected, state->sessions.size(), action == ListAction::Next);
                updateSelection(*state);
            }
            break;
        case ListAction::Open:
            if (rowsShown && !state->loading) {
                openSession(state, state->sessions[state->selected]);
            }
            break;
        case ListAction::Back:
            tt::app::stop();
            break;
        case ListAction::PickModel:
            if (state->loading) break;
            if (!state->models || state->models->choices.empty()) {
                showToast(*state, "Models unavailable");
                updateBanner(*state);
                break;
            }
            {
                auto& choices = state->models->choices;
                auto it = std::ranges::find(choices, state->models->current);
                state->modelSelected = it == choices.end() ? 0 : static_cast<size_t>(it - choices.begin());
            }
            state->picking = true;
            refreshSessionList(*state);
            break;
        case ListAction::NewSession: {
            if (state->client == nullptr || state->loading) break;
            auto client = state->client;
            auto backend = state->backend;
            auto created = std::make_shared<std::optional<BridgeClient::SessionSummary>>();
            auto error = std::make_shared<std::string>();
            state->loading = true;
            refreshSessionList(*state);
            runRequest(state, [client, backend, created, error] {
                *created = client->createSession(backend);
                if (!*created) *error = client->lastError();
            }, [state, created, error] {
                state->loading = false;
                if (*created) {
                    openSession(state, **created);
                } else {
                    showToast(*state, error->empty() ? "Could not create session" : *error);
                    updateBanner(*state);
                    refreshSessionList(*state);
                }
            });
            break;
        }
        case ListAction::None:
            break;
    }
}

} // namespace agents
