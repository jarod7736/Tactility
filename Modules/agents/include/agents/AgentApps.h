#pragma once

#include "agents/BridgeClient.h"
#include "agents/Card.h"

#include <Tactility/app/AppManifest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agents {

/** appId "HermesAgent", appName "Hermes" (backend "hermes"). Registered via tt::Configuration::apps. */
extern const tt::app::AppManifest hermesAppManifest;
/** appId "OpenCodeAgent", appName "OpenCode" (backend "opencode"). */
extern const tt::app::AppManifest openCodeAppManifest;

// region Pure helpers for the screens (hardware-free, exposed for tests)

/** "Hermes" for "hermes", "OpenCode" for "opencode", otherwise the id. */
std::string backendDisplayName(std::string_view backendId);

/** Session list header: "<Backend> • <model label>", or just the backend when the label is empty.
 *  U+2022 is used because the Montserrat fonts carry no U+00B7. */
std::string sessionListHeader(std::string_view backendName, std::string_view modelLabel);

/** Text shown instead of session rows, or empty when the rows should show.
 *  backend is this backend's entry from backends(), or nullptr when unknown. */
std::string sessionListMessage(bool statusOk, std::string_view statusMessage, const BridgeClient::BackendInfo* backend, size_t sessionCount);

/** Card screen header: "<Backend> • <session title> • <pos>/<size>" (title omitted when empty). */
std::string cardHeader(std::string_view backendName, std::string_view sessionTitle, size_t position, size_t size);

/** 'i' idle, 'r' running, 'w' needs you. */
const char* runStateWord(char state);

/** Run state after a bridge card. Only cards with seq >= lastSeq change it (a late "resolved" update for an
 *  older approval must not override a newer done card); lastSeq advances. seq 0 (gap card) changes nothing. */
char nextRunState(const Card& card, char current, int& lastSeq);

/** Card body text: title, body, numbered options for questions, "(truncated)". */
std::string cardBodyText(const Card& card);

/** Footer key hint for the current card (nullptr when the window is empty). */
std::string footerHint(const Card* card);

/** Banner text, or empty to hide it. Priority: toast, then bridge status, then the detached hint. */
std::string cardBanner(bool statusOk, std::string_view statusMessage, std::string_view toast, bool detached, size_t pendingNew);

enum class CardAction { None, Prev, Next, Back, Prompt, Stop, Respond, TypeAnswer };

struct CardKey {
    CardAction action = CardAction::None;
    std::string choice; // for Respond: the option key ("Y", "A", "N", "1".."9")
};

/** Card screen key mapping. key is an LVGL key code or a character. Fn+, / Fn+/ (LV_KEY_LEFT/RIGHT) map to
 *  None: LVGL scrolls the focused card body itself. */
CardKey cardKeyAction(uint32_t key, const Card* card, bool followingLatest);

/** Key repeat guard: P/T/S are ignored while a dialog is pending, Respond while a respond request for the
 *  current card is in flight. */
bool cardKeyBlocked(CardAction action, bool dialogPending, bool respondInFlight);

enum class OlderResult { Prepend, StartReached, Failed };

/** nullopt (request failed) is Failed and may be retried; an empty list means the start of history. */
OlderResult classifyOlder(const std::optional<std::vector<Card>>& cards);

/** Internal heap thresholds for shrinking the card window to 10: free < 30 KB or largest block < 16 KB. */
bool isLowMemory(size_t freeBytes, size_t largestBlock);

enum class ListAction { None, Prev, Next, Open, NewSession, PickModel, Back };

/** Session list (and model picker) key mapping. */
ListAction listKeyAction(uint32_t key);

// endregion

} // namespace agents
