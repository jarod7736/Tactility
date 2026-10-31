#include "agents/AgentApps.h"

#include "AgentState.h"

#include <Tactility/Tactility.h>
#include <Tactility/app/App.h>
#include <Tactility/app/AppContext.h>
#include <Tactility/app/alertdialog/AlertDialog.h>
#include <Tactility/app/inputdialog/InputDialog.h>
#include <Tactility/lvgl/LvglSync.h>

#include <tactility/lvgl_fonts.h>

#include <format>

namespace agents {

constexpr uint32_t TOAST_MILLIS = 3000;
constexpr uint32_t TIMER_MILLIS = 500;

// region Pure helpers

std::string backendDisplayName(std::string_view backendId) {
    if (backendId == "hermes") return "Hermes";
    if (backendId == "opencode") return "OpenCode";
    return std::string(backendId);
}

// endregion

// region View plumbing

bool isShown(const AgentState& state) {
    return state.parent != nullptr;
}

void showToast(AgentState& state, std::string text) {
    state.toast = std::move(text);
    state.toastUntil = lv_tick_get() + TOAST_MILLIS;
}

void updateBanner(AgentState& state) {
    if (state.banner == nullptr) return;
    if (!state.toast.empty() && static_cast<int32_t>(lv_tick_get() - state.toastUntil) >= 0) {
        state.toast.clear(); // expired
    }
    std::string text;
    if (state.mode == AgentState::Mode::Card) {
        bool ok = state.client != nullptr && state.client->status() == BridgeClient::Status::Ok;
        std::string message = state.client != nullptr ? state.client->statusMessage() : "Bridge service not running";
        text = cardBanner(ok, message, state.toast, state.window.detached(), state.window.pendingNew());
    } else {
        text = state.toast;
    }
    if (text.empty()) {
        lv_obj_add_flag(state.banner, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (text != lv_label_get_text(state.banner)) {
            lv_label_set_text(state.banner, text.c_str());
        }
        lv_obj_remove_flag(state.banner, LV_OBJ_FLAG_HIDDEN);
    }
}

static void onTimer(lv_timer_t* timer) {
    auto* state = static_cast<AgentState*>(lv_timer_get_user_data(timer));
    updateBanner(*state);
}

static void onKey(lv_event_t* event) {
    auto* state = static_cast<AgentState*>(lv_event_get_user_data(event));
    uint32_t key = lv_event_get_key(event);
    auto self = state->shared_from_this();
    if (state->mode == AgentState::Mode::List) {
        handleListKey(self, key);
    } else {
        handleCardKey(self, key);
    }
}

/** The shown agent app (one at a time). LVGL has no group user data accessor in this version. */
static AgentState* edgeTarget = nullptr;

/** NEXT/PREV only move group focus in LVGL; with one object in the group they end up here. */
static void onEdge(lv_group_t* group, bool next) {
    auto* state = edgeTarget;
    if (state == nullptr || state->group != group) return;
    auto self = state->shared_from_this();
    uint32_t key = next ? LV_KEY_NEXT : LV_KEY_PREV;
    if (state->mode == AgentState::Mode::List) {
        handleListKey(self, key);
    } else {
        handleCardKey(self, key);
    }
}

static lv_obj_t* createSmallLabel(lv_obj_t* parent) {
    auto* label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_label_set_text(label, "");
    return label;
}

static void buildSkeleton(AgentState& state) {
    auto* parent = state.parent;
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 2, 0);
    lv_obj_set_style_pad_row(parent, 1, 0);

    auto* header = lv_obj_create(parent);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(header, 4, 0);

    state.headerLeft = createSmallLabel(header);
    lv_obj_set_flex_grow(state.headerLeft, 1);
    lv_label_set_long_mode(state.headerLeft, LV_LABEL_LONG_DOT);
    state.headerRight = createSmallLabel(header);

    state.banner = createSmallLabel(parent);
    lv_obj_set_width(state.banner, LV_PCT(100));
    lv_label_set_long_mode(state.banner, LV_LABEL_LONG_DOT);
    lv_obj_set_style_bg_color(state.banner, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_set_style_bg_opa(state.banner, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(state.banner, lv_color_black(), 0);
    lv_obj_add_flag(state.banner, LV_OBJ_FLAG_HIDDEN);

    state.content = lv_obj_create(parent);
    lv_obj_set_width(state.content, LV_PCT(100));
    lv_obj_set_flex_grow(state.content, 1);
    lv_obj_set_style_pad_all(state.content, 2, 0);
    lv_obj_set_style_border_width(state.content, 0, 0);
    lv_obj_set_style_outline_width(state.content, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_scroll_dir(state.content, LV_DIR_VER);
    lv_obj_add_event_cb(state.content, onKey, LV_EVENT_KEY, &state);

    state.footer = createSmallLabel(parent);
    lv_obj_set_width(state.footer, LV_PCT(100));
    lv_label_set_long_mode(state.footer, LV_LABEL_LONG_DOT);

    // GuiService made a fresh default group for this app; the content object is its only member.
    state.group = lv_group_get_default();
    if (state.group != nullptr) {
        lv_group_remove_all_objs(state.group);
        lv_group_add_obj(state.group, state.content);
        lv_group_set_wrap(state.group, false);
        lv_group_set_edge_cb(state.group, onEdge);
        edgeTarget = &state;
        lv_group_focus_obj(state.content);
    }

    state.timer = lv_timer_create(onTimer, TIMER_MILLIS, &state);
}

void buildView(const StatePtr& state) {
    state->generation++;
    buildSkeleton(*state);
    if (state->mode == AgentState::Mode::List) {
        buildSessionList(state);
    } else {
        buildCardView(state);
    }
    updateBanner(*state);
}

void teardownView(AgentState& state) {
    state.generation++;
    if (state.timer != nullptr) {
        lv_timer_delete(state.timer);
        state.timer = nullptr;
    }
    if (state.group != nullptr) {
        lv_group_set_edge_cb(state.group, nullptr);
        state.group = nullptr;
    }
    if (edgeTarget == &state) {
        edgeTarget = nullptr;
    }
    state.parent = nullptr;
    state.headerLeft = state.headerRight = state.banner = state.content = state.bodyLabel = state.footer = nullptr;
    state.shownSeq = -2;
}

void rebuildLater(const StatePtr& state) {
    auto* holder = new std::weak_ptr<AgentState>(state);
    lv_async_call([](void* data) {
        auto* weak = static_cast<std::weak_ptr<AgentState>*>(data);
        if (auto state = weak->lock(); state && isShown(*state)) {
            auto* parent = state->parent;
            teardownView(*state);
            lv_obj_clean(parent);
            state->parent = parent;
            buildView(state);
        }
        delete weak;
    }, holder);
}

void runRequest(const StatePtr& state, std::function<void()> work, std::function<void()> apply) {
    tt::getMainDispatcher().dispatch([state, work = std::move(work), apply = std::move(apply)] {
        work();
        if (tt::lvgl::lock(portMAX_DELAY)) {
            apply();
            tt::lvgl::unlock();
        }
    });
}

// endregion

// region App

class AgentApp final : public tt::app::App {
    StatePtr state = std::make_shared<AgentState>();

public:
    explicit AgentApp(std::string backendId) {
        state->name = backendDisplayName(backendId);
        state->backend = std::move(backendId);
    }

    void onCreate(tt::app::AppContext& context) override {
        state->client = findBridgeClient();
    }

    void onDestroy(tt::app::AppContext& context) override {
        // watch()/unwatch() must run with the LVGL lock held (BridgeClient.h)
        if (tt::lvgl::lock(portMAX_DELAY)) {
            stopWatching(*state);
            tt::lvgl::unlock();
        }
    }

    void onShow(tt::app::AppContext& context, lv_obj_t* parent) override {
        state->parent = parent;
        buildView(state);
    }

    void onHide(tt::app::AppContext& context) override {
        teardownView(*state);
    }

    void onResult(tt::app::AppContext& context, tt::app::LaunchId launchId, tt::app::Result result, std::unique_ptr<tt::Bundle> resultData) override {
        bool ok = result == tt::app::Result::Ok && resultData != nullptr;
        std::string text = ok ? tt::app::inputdialog::getResult(*resultData) : std::string();
        int32_t index = ok ? tt::app::alertdialog::getResultIndex(*resultData) : -1;
        handleDialogResult(state, ok, std::move(text), index);
    }
};

static std::shared_ptr<tt::app::App> createHermes() {
    return std::make_shared<AgentApp>("hermes");
}

static std::shared_ptr<tt::app::App> createOpenCode() {
    return std::make_shared<AgentApp>("opencode");
}

extern const tt::app::AppManifest hermesAppManifest = {
    .appId = "HermesAgent",
    .appName = "Hermes",
    .appCategory = tt::app::Category::User,
    .createApp = createHermes
};

extern const tt::app::AppManifest openCodeAppManifest = {
    .appId = "OpenCodeAgent",
    .appName = "OpenCode",
    .appCategory = tt::app::Category::User,
    .createApp = createOpenCode
};

// endregion

} // namespace agents
