#include "doctest.h"
#include <agents/Card.h>

using namespace agents;

TEST_CASE("parseCard reads the spec approval card") {
    auto card = parseCard(R"({"seq": 42, "kind": "approval", "title": "Permission: bash",
 "body": "npm test -- auth",
 "options": [{"key": "Y", "label": "allow"}, {"key": "A", "label": "always"},
             {"key": "N", "label": "deny"}],
 "truncated": false, "resolved": false, "ts": 1793410000})");
    REQUIRE(card.has_value());
    CHECK_EQ(card->seq, 42);
    CHECK(card->kind == CardKind::Approval);
    CHECK_EQ(card->title, "Permission: bash");
    CHECK_EQ(card->body, "npm test -- auth");
    REQUIRE_EQ(card->options.size(), 3);
    CHECK_EQ(card->options[0].key, 'Y');
    CHECK_EQ(card->options[0].label, "allow");
    CHECK_EQ(card->options[1].key, 'A');
    CHECK_EQ(card->options[2].key, 'N');
    CHECK_FALSE(card->truncated);
    CHECK_FALSE(card->resolved);
    CHECK_EQ(card->ts, 1793410000);
}

TEST_CASE("parseCard maps every kind") {
    struct { const char* name; CardKind kind; } cases[] = {
        {"user", CardKind::User}, {"step", CardKind::Step}, {"text", CardKind::Text},
        {"approval", CardKind::Approval}, {"question", CardKind::Question},
        {"done", CardKind::Done}, {"error", CardKind::Error},
    };
    for (auto& c : cases) {
        auto card = parseCard(std::string(R"({"seq":1,"kind":")") + c.name + R"("})");
        REQUIRE(card.has_value());
        CHECK(card->kind == c.kind);
    }
}

TEST_CASE("parseCard rejects unknown kind and invalid JSON") {
    CHECK_FALSE(parseCard(R"({"seq":1,"kind":"bogus"})").has_value());
    CHECK_FALSE(parseCard(R"({"seq":1})").has_value());
    CHECK_FALSE(parseCard("not json").has_value());
    CHECK_FALSE(parseCard("").has_value());
    CHECK_FALSE(parseCard("[1,2]").has_value());
}

TEST_CASE("parseCard defaults missing optional fields") {
    auto card = parseCard(R"({"seq":7,"kind":"step"})");
    REQUIRE(card.has_value());
    CHECK_EQ(card->title, "");
    CHECK_EQ(card->body, "");
    CHECK(card->options.empty());
    CHECK_FALSE(card->truncated);
    CHECK_FALSE(card->resolved);
    CHECK_EQ(card->ts, 0);
}

TEST_CASE("parseCard keeps raw UTF-8 and skips malformed options") {
    auto card = parseCard(R"({"seq":1,"kind":"question","title":"Choisir — é","options":[{"key":"1","label":"oui"},{"key":"","label":"x"},{"key":"22","label":"y"},{"label":"z"},"junk"],"truncated":true,"resolved":true})");
    REQUIRE(card.has_value());
    CHECK_EQ(card->title, "Choisir — é");
    REQUIRE_EQ(card->options.size(), 1);
    CHECK_EQ(card->options[0].key, '1');
    CHECK(card->truncated);
    CHECK(card->resolved);
}
