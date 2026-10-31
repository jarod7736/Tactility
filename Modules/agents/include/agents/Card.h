#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agents {

enum class CardKind { User, Step, Text, Approval, Question, Done, Error };

struct CardOption {
    char key;
    std::string label;
};

struct Card {
    int seq = 0;
    CardKind kind = CardKind::Text;
    std::string title, body;
    std::vector<CardOption> options;
    bool truncated = false;
    bool resolved = false;
    int64_t ts = 0;
    /** Made on the device, not by the bridge (e.g. "card too large"). The UI shows a local card as a
     *  transient banner/toast and never passes it to CardWindow::apply: its seq is not a bridge seq. */
    bool local = false;
};

/** Parse one card JSON object. Returns nullopt on invalid JSON, missing seq/kind, or unknown kind.
 *  Options whose key is not exactly one character are skipped. */
std::optional<Card> parseCard(std::string_view json);

} // namespace agents
