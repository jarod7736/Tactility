#include "agents/AgentApps.h"

#include "AgentState.h"

#include <Tactility/app/alertdialog/AlertDialog.h>
#include <Tactility/app/inputdialog/InputDialog.h>
#include <Tactility/lvgl/LvglSync.h>

#include <cctype>
#include <format>

namespace agents {

// region Pure helpers

std::string cardHeader(std::string_view backendName, std::string_view sessionTitle, size_t position, size_t size) {
    if (sessionTitle.empty()) return std::format("{} • {}/{}", backendName, position, size);
    return std::format("{} • {} • {}/{}", backendName, sessionTitle, position, size);
}

const char* runStateWord(char state) {
    switch (state) {
        case 'r': return "running";
        case 'w': return "needs you";
        default: return "idle";
    }
}

char nextRunState(const Card& card, char current, int& lastSeq) {
    if (card.seq <= 0 || card.seq < lastSeq) return current;
    lastSeq = card.seq;
    switch (card.kind) {
        case CardKind::Approval:
        case CardKind::Question:
            return card.resolved ? 'r' : 'w';
        case CardKind::Done:
        case CardKind::Error:
            return 'i';
        default:
            return 'r';
    }
}

std::string cardBodyText(const Card& card) {
    std::string text = card.title;
    if (!card.body.empty()) {
        if (!text.empty()) text += '\n';
        text += card.body;
    }
    if (card.kind == CardKind::Question) {
        for (const auto& option : card.options) {
            text += std::format("\n{}. {}", option.key, option.label);
        }
    }
    if (card.truncated) text += "\n(truncated)";
    return text;
}

static const char* approvalWord(char key) {
    switch (key) {
        case 'Y': return "allow";
        case 'A': return "always";
        case 'N': return "deny";
        default: return nullptr;
    }
}

static const CardOption* findOption(const Card& card, char key) {
    for (const auto& option : card.options) {
        if (std::toupper(static_cast<unsigned char>(option.key)) == std::toupper(static_cast<unsigned char>(key))) {
            return &option;
        }
    }
    return nullptr;
}

std::string footerHint(const Card* card) {
    if (card == nullptr) return "[P] prompt";
    if (card->kind == CardKind::Approval || card->kind == CardKind::Question) {
        if (card->resolved) return "resolved";
        std::string hint;
        if (card->kind == CardKind::Approval) {
            for (const auto& option : card->options) {
                auto* word = approvalWord(static_cast<char>(std::toupper(static_cast<unsigned char>(option.key))));
                if (word == nullptr) continue;
                if (!hint.empty()) hint += ' ';
                hint += std::format("[{}] {}", static_cast<char>(std::toupper(static_cast<unsigned char>(option.key))), word);
            }
            return hint;
        }
        auto count = card->options.size();
        if (count == 0) return "[T] type answer";
        if (count == 1) return "[1] pick [T] type";
        return std::format("[1-{}] pick [T] type", count);
    }
    if (card->kind == CardKind::User) return "[E] edit [P] prompt [S] stop";
    return "[P] prompt [S] stop";
}

std::string cardBanner(bool statusOk, std::string_view statusMessage, std::string_view toast, bool detached, size_t pendingNew) {
    if (!toast.empty()) return std::string(toast);
    if (!statusOk) return std::string(statusMessage);
    if (detached) {
        if (pendingNew == 0) return "Fn+. at end to jump to latest";
        return std::format("{} new - Fn+. at end to jump", pendingNew);
    }
    return {};
}

CardKey cardKeyAction(uint32_t key, const Card* card, bool followingLatest) {
    switch (key) {
        case LV_KEY_PREV: return { CardAction::Prev, {} };
        case LV_KEY_NEXT: return { CardAction::Next, {} };
        case LV_KEY_ESC: return { CardAction::Back, {} };
        case LV_KEY_ENTER: return { followingLatest ? CardAction::Prompt : CardAction::None, {} };
        case 'p':
        case 'P': return { CardAction::Prompt, {} };
        case 's':
        case 'S': return { CardAction::Stop, {} };
        default: break;
    }
    if (card == nullptr || card->resolved || key > 0x7F) return {};
    char c = static_cast<char>(std::toupper(static_cast<int>(key)));
    if (card->kind == CardKind::User && c == 'E') return { CardAction::Edit, {} };
    if (card->kind == CardKind::Approval) {
        if (approvalWord(c) == nullptr) return {};
        if (auto* option = findOption(*card, c)) return { CardAction::Respond, std::string(1, option->key) };
    } else if (card->kind == CardKind::Question) {
        if (c == 'T') return { CardAction::TypeAnswer, {} };
        if (c >= '1' && c <= '9') {
            if (auto* option = findOption(*card, c)) return { CardAction::Respond, std::string(1, option->key) };
        }
    }
    return {};
}

bool cardKeyBlocked(CardAction action, bool dialogPending, bool respondInFlight) {
    switch (action) {
        case CardAction::Prompt:
        case CardAction::Edit:
        case CardAction::TypeAnswer:
        case CardAction::Stop:
            return dialogPending;
        case CardAction::Respond:
            return respondInFlight;
        default:
            return false;
    }
}

OlderResult classifyOlder(const std::optional<std::vector<Card>>& cards) {
    if (!cards) return OlderResult::Failed;
    return cards->empty() ? OlderResult::StartReached : OlderResult::Prepend;
}

// endregion

void refreshCardView(AgentState& state) {
    // viewMode/bodyLabel: openSession() switches mode before the async rebuild creates the card widgets
    if (!isShown(state) || state.mode != AgentState::Mode::Card || state.viewMode != AgentState::Mode::Card || state.bodyLabel == nullptr) return;
    const Card* current = state.window.current();
    lv_label_set_text(state.headerLeft, cardHeader(state.name, state.session.title, state.window.position(), state.window.size()).c_str());
    lv_label_set_text(state.headerRight, runStateWord(state.runState));
    int seq = current != nullptr ? current->seq : -1;
    bool resolved = current != nullptr && current->resolved;
    if (current == nullptr) {
        lv_label_set_text(state.bodyLabel, state.watching ? "Waiting for cards..." : "Not connected");
    } else if (seq != state.shownSeq || resolved != state.shownResolved) {
        lv_label_set_text(state.bodyLabel, cardBodyText(*current).c_str());
    }
    if (seq != state.shownSeq) {
        lv_obj_scroll_to_y(state.content, 0, LV_ANIM_OFF);
    }
    state.shownSeq = seq;
    state.shownResolved = resolved;
    lv_label_set_text(state.footer, footerHint(current).c_str());
    updateBanner(state);
}

void buildCardView(const StatePtr& state) {
    state->bodyLabel = lv_label_create(state->content);
    lv_obj_set_width(state->bodyLabel, LV_PCT(100));
    lv_label_set_long_mode(state->bodyLabel, LV_LABEL_LONG_WRAP);
    refreshCardView(*state);
}

/** Runs on the main dispatcher with the LVGL lock held (BridgeClient::watch). */
static void onCard(AgentState& state, const Card& card) {
    if (card.local) {
        // e.g. "card too large": not a bridge seq, so it never enters the window
        showToast(state, card.title);
        updateBanner(state);
        return;
    }
    auto change = state.window.apply(card);
    if (change == CardWindow::Change::Reset) {
        state.runStateSeq = 0;
        state.historyStart = false;
        state.shownSeq = -2; // same seq may now be a different card
    }
    state.runState = nextRunState(card, state.runState, state.runStateSeq);
    refreshCardView(state);
}

static void startWatching(const StatePtr& state) {
    if (state->client == nullptr || state->destroyed) return;
    std::weak_ptr<AgentState> weak = state;
    state->client->watch(state->backend, state->session.id, 0, [weak](const Card& card) {
        if (auto locked = weak.lock()) onCard(*locked, card);
    });
    state->watching = true;
}

void stopWatching(AgentState& state) {
    if (state.watching && state.client != nullptr) {
        state.client->unwatch();
    }
    state.watching = false;
}

void openSession(const StatePtr& state, BridgeClient::SessionSummary session) {
    if (state->destroyed) return; // e.g. createSession finished after the app exited
    stopWatching(*state);
    state->mode = AgentState::Mode::Card;
    state->session = std::move(session);
    state->window.clear();
    state->runState = state->session.state;
    state->runStateSeq = 0;
    state->fetchingOlder = false;
    state->historyStart = false;
    state->respondingSeq = -1;
    startWatching(state);
    rebuildLater(state);
}

void closeSession(const StatePtr& state) {
    stopWatching(*state);
    state->window.clear();
    state->mode = AgentState::Mode::List;
    rebuildLater(state);
}

/** "Jump to latest" while detached: drop the window and replay the stream from the start. */
static void jumpToLatest(const StatePtr& state) {
    state->window.clear();
    state->runStateSeq = 0;
    state->historyStart = false;
    stopWatching(*state);
    startWatching(state);
}

static void fetchOlder(const StatePtr& state) {
    int before = state->window.oldestSeq();
    if (state->fetchingOlder || before <= 0 || state->client == nullptr) return; // never send before=-1
    if (state->historyStart) {
        showToast(*state, "No older cards");
        updateBanner(*state);
        return;
    }
    state->fetchingOlder = true;
    auto client = state->client;
    auto backend = state->backend;
    auto sid = state->session.id;
    auto cards = std::make_shared<std::optional<std::vector<Card>>>();
    auto error = std::make_shared<std::string>();
    runRequest(state, [client, backend, sid, before, cards, error] {
        *cards = client->older(backend, sid, before);
        if (!*cards) {
            *error = client->lastError();
            if (error->empty()) *error = client->statusMessage();
        }
    }, [state, sid, before, cards, error] {
        state->fetchingOlder = false;
        if (state->mode != AgentState::Mode::Card || state->session.id != sid || state->window.oldestSeq() != before) return;
        switch (classifyOlder(*cards)) {
            case OlderResult::Failed:
                showToast(*state, *error); // not remembered: Fn+; retries
                break;
            case OlderResult::StartReached:
                state->historyStart = true;
                showToast(*state, "No older cards");
                break;
            case OlderResult::Prepend:
                state->window.prependOlder(std::move(**cards));
                state->window.prev();
                break;
        }
        refreshCardView(*state);
    });
}

static void sendRequest(const StatePtr& state, std::function<bool(BridgeClient&)> request, const char* fallbackError, std::function<void()> onDone = {}) {
    if (state->client == nullptr) return;
    auto client = state->client;
    auto error = std::make_shared<std::string>();
    runRequest(state, [client, request = std::move(request), error, fallbackError] {
        if (!request(*client)) {
            *error = client->lastError();
            if (error->empty()) *error = fallbackError;
        }
    }, [state, error, onDone] {
        if (onDone) onDone();
        if (!error->empty()) {
            showToast(*state, *error);
            updateBanner(*state);
        }
    });
}

/** Call with the LVGL lock held. */
static void respond(const StatePtr& state, std::string backend, std::string sid, int seq, std::optional<std::string> choice, std::optional<std::string> answer) {
    state->respondingSeq = seq;
    sendRequest(state, [backend, sid, seq, choice, answer](BridgeClient& client) {
        return client.respond(backend, sid, seq, choice, answer);
    }, "Could not respond", [state, seq] {
        if (state->respondingSeq == seq) state->respondingSeq = -1;
    });
}

void handleCardKey(const StatePtr& state, uint32_t key) {
    const Card* current = state->window.current();
    auto action = cardKeyAction(key, current, state->window.followingLatest());
    bool respondInFlight = current != nullptr && state->respondingSeq == current->seq;
    if (cardKeyBlocked(action.action, state->dialog != AgentState::Dialog::None, respondInFlight)) return;
    switch (action.action) {
        case CardAction::Prev:
            if (!state->window.prev()) fetchOlder(state);
            refreshCardView(*state);
            break;
        case CardAction::Next:
            if (!state->window.next() && state->window.detached()) jumpToLatest(state);
            refreshCardView(*state);
            break;
        case CardAction::Back:
            closeSession(state);
            break;
        case CardAction::Prompt:
            state->dialog = AgentState::Dialog::Prompt;
            tt::app::inputdialog::start("Prompt", std::format("{}: {}", state->name, state->session.title));
            break;
        case CardAction::Edit:
            state->dialog = AgentState::Dialog::Prompt;
            tt::app::inputdialog::start("Prompt", std::format("{}: {}", state->name, state->session.title), current->body);
            break;
        case CardAction::Stop:
            state->dialog = AgentState::Dialog::Stop;
            tt::app::alertdialog::start("Stop run?", std::format("Stop the current {} run?", state->name), std::vector<std::string> { "Yes", "No" });
            break;
        case CardAction::Respond:
            respond(state, state->backend, state->session.id, current->seq, action.choice, std::nullopt);
            break;
        case CardAction::TypeAnswer:
            state->dialog = AgentState::Dialog::Answer;
            state->answerSeq = current->seq;
            tt::app::inputdialog::start("Answer", current->title);
            break;
        case CardAction::None:
            break;
    }
}

void handleDialogResult(const StatePtr& state, bool ok, std::string text, int32_t buttonIndex) {
    AgentState::Dialog dialog;
    int answerSeq;
    std::string backend, sid;
    if (!tt::lvgl::lock(portMAX_DELAY)) return;
    dialog = state->dialog;
    answerSeq = state->answerSeq;
    backend = state->backend;
    sid = state->session.id;
    state->dialog = AgentState::Dialog::None;
    bool inSession = state->mode == AgentState::Mode::Card;
    tt::lvgl::unlock();
    if (!ok || !inSession) return;

    switch (dialog) {
        case AgentState::Dialog::Prompt:
            if (text.empty()) return;
            sendRequest(state, [backend, sid, text](BridgeClient& client) {
                return client.prompt(backend, sid, text);
            }, "Could not send prompt");
            break;
        case AgentState::Dialog::Answer:
            if (text.empty()) return;
            if (tt::lvgl::lock(portMAX_DELAY)) {
                respond(state, backend, sid, answerSeq, std::nullopt, text);
                tt::lvgl::unlock();
            }
            break;
        case AgentState::Dialog::Stop:
            if (buttonIndex != 0) return; // 0 = Yes
            sendRequest(state, [backend, sid](BridgeClient& client) {
                return client.stop(backend, sid);
            }, "Could not stop");
            break;
        case AgentState::Dialog::None:
            break;
    }
}

} // namespace agents
