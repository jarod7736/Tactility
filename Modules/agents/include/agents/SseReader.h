#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace agents {

struct SseEvent {
    enum class Type { Data, Oversize } type;
    std::string data; // payload of a Data event; empty for Oversize
};

/**
 * Incremental Server-Sent Events line reader. Emits one Data event per non-empty
 * `data:` line. Comments, other fields and blank lines are ignored.
 * The internal buffer never exceeds maxLine bytes: a longer line yields exactly one
 * Oversize event and the rest of that line is discarded up to the next newline.
 */
class SseReader {
    size_t maxLine;
    std::string line;
    bool discarding = false;

    void handleLine(const std::function<void(SseEvent)>& onEvent);

public:
    explicit SseReader(size_t maxLine = 2048) : maxLine(maxLine) {}

    void feed(std::string_view bytes, const std::function<void(SseEvent)>& onEvent);
};

} // namespace agents
