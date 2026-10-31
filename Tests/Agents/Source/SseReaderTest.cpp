#include "doctest.h"
#include <agents/SseReader.h>
#include <vector>

using namespace agents;

namespace {
std::vector<SseEvent> feedAll(SseReader& reader, std::string_view bytes) {
    std::vector<SseEvent> events;
    reader.feed(bytes, [&](SseEvent e) { events.push_back(std::move(e)); });
    return events;
}
}

TEST_CASE("SseReader emits one Data event for data line plus blank line") {
    SseReader reader;
    auto events = feedAll(reader, "data: {\"a\":1}\n\n");
    REQUIRE_EQ(events.size(), 1);
    CHECK(events[0].type == SseEvent::Type::Data);
    CHECK_EQ(events[0].data, "{\"a\":1}");
}

TEST_CASE("SseReader handles CRLF framing") {
    SseReader reader;
    auto events = feedAll(reader, "data: x\r\n\r\ndata: y\r\n\r\n");
    REQUIRE_EQ(events.size(), 2);
    CHECK_EQ(events[0].data, "x");
    CHECK_EQ(events[1].data, "y");
}

TEST_CASE("SseReader reassembles a line split across feed calls") {
    SseReader reader;
    CHECK(feedAll(reader, "data: {\"se").empty());
    CHECK(feedAll(reader, "q\":1}").empty());
    auto events = feedAll(reader, "\n\n");
    REQUIRE_EQ(events.size(), 1);
    CHECK_EQ(events[0].data, "{\"seq\":1}");
}

TEST_CASE("SseReader splits CRLF across feed calls") {
    SseReader reader;
    CHECK(feedAll(reader, "data: x\r").empty());
    auto events = feedAll(reader, "\n\r\n");
    REQUIRE_EQ(events.size(), 1);
    CHECK_EQ(events[0].data, "x");
}

TEST_CASE("SseReader ignores comment lines") {
    SseReader reader;
    CHECK(feedAll(reader, ": keepalive\n\n").empty());
    auto events = feedAll(reader, ": keepalive\n\ndata: z\n\n");
    REQUIRE_EQ(events.size(), 1);
    CHECK_EQ(events[0].data, "z");
}

TEST_CASE("SseReader ignores empty data and unknown fields") {
    SseReader reader;
    CHECK(feedAll(reader, "data:\n\n").empty());
    CHECK(feedAll(reader, "data: \n\n").empty());
    CHECK(feedAll(reader, "event: card\nid: 5\nretry: 10\n\n").empty());
    auto events = feedAll(reader, "event: card\nid: 5\ndata:nospace\n\n");
    REQUIRE_EQ(events.size(), 1);
    CHECK_EQ(events[0].data, "nospace");
}

TEST_CASE("SseReader gives exactly one Oversize event for a 3000-byte line and recovers") {
    SseReader reader; // maxLine 2048
    std::string big = "data: " + std::string(3000, 'a') + "\n\n";
    auto events = feedAll(reader, big);
    REQUIRE_EQ(events.size(), 1);
    CHECK(events[0].type == SseEvent::Type::Oversize);
    events = feedAll(reader, "data: ok\n\n");
    REQUIRE_EQ(events.size(), 1);
    CHECK(events[0].type == SseEvent::Type::Data);
    CHECK_EQ(events[0].data, "ok");
}

TEST_CASE("SseReader oversize line spread over many small feeds") {
    SseReader reader(64);
    std::vector<SseEvent> all;
    auto collect = [&](SseEvent e) { all.push_back(std::move(e)); };
    reader.feed("data: ", collect);
    for (int i = 0; i < 100; i++) reader.feed("0123456789", collect);
    CHECK_EQ(all.size(), 1);
    reader.feed("\n\ndata: next\n\n", collect);
    REQUIRE_EQ(all.size(), 2);
    CHECK(all[0].type == SseEvent::Type::Oversize);
    CHECK_EQ(all[1].data, "next");
}

TEST_CASE("SseReader accepts a line of exactly maxLine bytes") {
    SseReader reader(16);
    auto events = feedAll(reader, "data: 0123456789\n\n"); // 16 bytes
    REQUIRE_EQ(events.size(), 1);
    CHECK(events[0].type == SseEvent::Type::Data);
    events = feedAll(reader, "data: 01234567890\n\n"); // 17 bytes
    REQUIRE_EQ(events.size(), 1);
    CHECK(events[0].type == SseEvent::Type::Oversize);
}
