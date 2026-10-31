#pragma once

// Private to the agents module: state shared by AgentApp, the session list and the card screen.

#include "agents/BridgeClient.h"
#include "agents/CardWindow.h"

#include <lvgl.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace agents {

/** All fields are read and written only with the LVGL lock held. Network calls run on the main dispatcher
 *  (see runRequest) and apply their results under the lock. */
struct AgentState : std::enable_shared_from_this<AgentState> {
    enum class Mode { List, Card };
    enum class Dialog { None, Prompt, Answer, Stop };

    std::string backend, name;
    std::shared_ptr<BridgeClient> client; // nullptr when the service is not running

    Mode mode = Mode::List;
    Mode viewMode = Mode::List; // the mode the current widgets were built for
    bool destroyed = false;     // set in onDestroy: late request results must not start a watch
    Dialog dialog = Dialog::None;
    int answerSeq = -1;

    // Widgets: non-null only while the app is shown. Tactility deletes them; teardownView() forgets them.
    lv_obj_t* parent = nullptr;
    lv_obj_t* headerLeft = nullptr;
    lv_obj_t* headerRight = nullptr;
    lv_obj_t* banner = nullptr;
    lv_obj_t* content = nullptr; // scrollable, focused: holds the rows or the card body
    lv_obj_t* bodyLabel = nullptr;
    lv_obj_t* footer = nullptr;
    lv_timer_t* timer = nullptr;
    lv_group_t* group = nullptr;
    uint32_t generation = 0; // bumped on every build/teardown

    std::string toast;
    uint32_t toastUntil = 0;

    // Session list
    bool loading = false;
    std::vector<BridgeClient::SessionSummary> sessions;
    std::optional<BridgeClient::BackendInfo> backendInfo;
    std::optional<BridgeClient::ModelList> models;
    std::string listMessage;
    size_t selected = 0;
    bool picking = false;
    size_t modelSelected = 0;

    // Card screen
    BridgeClient::SessionSummary session;
    CardWindow window;
    char runState = 'i';
    int runStateSeq = 0;
    bool watching = false;
    bool fetchingOlder = false;
    bool historyStart = false; // a successful empty older() response: nothing older to fetch
    int respondingSeq = -1;    // seq with a respond request in flight (key repeat guard)
    bool lowMemory = false;    // window shrunk to 10 (one-way)
    int shownSeq = -2;         // (seq, resolved) of the card in the body label: skip identical updates
    bool shownResolved = false;
};

using StatePtr = std::shared_ptr<AgentState>;

// AgentApps.cpp
/** Build the widgets for state.mode into state.parent. */
void buildView(const StatePtr& state);
/** Forget widgets (they are deleted by Tactility or lv_obj_clean). */
void teardownView(AgentState& state);
/** Rebuild the current view after the current LVGL event finishes (safe from key handlers). */
void rebuildLater(const StatePtr& state);
/** Run work() on the main dispatcher without the LVGL lock, then apply() with the lock held. */
void runRequest(const StatePtr& state, std::function<void()> work, std::function<void()> apply);
void showToast(AgentState& state, std::string text);
/** Show or hide the banner (toast, bridge status, detached hint). Also runs every 500 ms. */
void updateBanner(AgentState& state);
bool isShown(const AgentState& state);

// SessionListView.cpp
void buildSessionList(const StatePtr& state);
void refreshSessionList(AgentState& state);
void handleListKey(const StatePtr& state, uint32_t key);
void loadSessionList(const StatePtr& state);

// CardView.cpp
void buildCardView(const StatePtr& state);
void refreshCardView(AgentState& state);
void handleCardKey(const StatePtr& state, uint32_t key);
/** Open a session: clear the window and watch from the start. */
void openSession(const StatePtr& state, BridgeClient::SessionSummary session);
/** Stop watching and return to the list. */
void closeSession(const StatePtr& state);
void stopWatching(AgentState& state);
/** Handle an inputdialog / alertdialog result (called on the loader thread, without the LVGL lock). */
void handleDialogResult(const StatePtr& state, bool ok, std::string text, int32_t buttonIndex);

} // namespace agents
