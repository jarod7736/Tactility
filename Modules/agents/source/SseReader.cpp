#include "agents/SseReader.h"

namespace agents {

void SseReader::handleLine(const std::function<void(SseEvent)>& onEvent) {
    constexpr std::string_view prefix = "data:";
    std::string_view view = line;
    if (view.substr(0, prefix.size()) == prefix) {
        view.remove_prefix(prefix.size());
        if (!view.empty() && view.front() == ' ') {
            view.remove_prefix(1);
        }
        if (!view.empty()) {
            onEvent(SseEvent { SseEvent::Type::Data, std::string(view) });
        }
    }
    line.clear();
}

void SseReader::feed(std::string_view bytes, const std::function<void(SseEvent)>& onEvent) {
    for (char c : bytes) {
        if (c == '\n') {
            if (discarding) {
                discarding = false;
                line.clear();
            } else {
                handleLine(onEvent);
            }
        } else if (c == '\r' || discarding) {
            continue;
        } else if (line.size() >= maxLine) {
            line.clear();
            discarding = true;
            onEvent(SseEvent { SseEvent::Type::Oversize, {} });
        } else {
            line.push_back(c);
        }
    }
}

} // namespace agents
