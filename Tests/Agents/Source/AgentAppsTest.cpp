#include <doctest.h>

#include <agents/AgentApps.h>

#include <lvgl.h>

using namespace agents;

namespace {
Card mk(int seq, CardKind kind, bool resolved = false) {
    Card c;
    c.seq = seq;
    c.kind = kind;
    c.resolved = resolved;
    return c;
}
Card approval(std::vector<char> keys, bool resolved = false) {
    Card c = mk(5, CardKind::Approval, resolved);
    for (char k : keys) c.options.push_back({ k, "x" });
    return c;
}
Card question(int optionCount, bool resolved = false) {
    Card c = mk(6, CardKind::Question, resolved);
    for (int i = 1; i <= optionCount; i++) c.options.push_back({ static_cast<char>('0' + i), "opt" + std::to_string(i) });
    return c;
}
} // namespace

TEST_CASE("app manifests") {
    CHECK(hermesAppManifest.appId == "HermesAgent");
    CHECK(hermesAppManifest.appName == "Hermes");
    CHECK(openCodeAppManifest.appId == "OpenCodeAgent");
    CHECK(openCodeAppManifest.appName == "OpenCode");
    CHECK(hermesAppManifest.createApp != nullptr);
    CHECK(openCodeAppManifest.createApp != nullptr);
    CHECK(backendDisplayName("hermes") == "Hermes");
    CHECK(backendDisplayName("opencode") == "OpenCode");
    CHECK(backendDisplayName("other") == "other");
}

TEST_CASE("session list header and message") {
    CHECK(sessionListHeader("Hermes", "gpt-5 (Gufo)") == "Hermes • gpt-5 (Gufo)");
    CHECK(sessionListHeader("OpenCode", "") == "OpenCode");

    BridgeClient::BackendInfo down { "hermes", "Hermes", false, "Hermes unreachable" };
    BridgeClient::BackendInfo up { "hermes", "Hermes", true, "" };
    CHECK(sessionListMessage(false, "Bridge offline, retrying", &up, 3) == "Bridge offline, retrying");
    CHECK(sessionListMessage(true, "Connected", &down, 3) == "Hermes unreachable");
    down.reason = "";
    CHECK(sessionListMessage(true, "Connected", &down, 3) == "Backend unavailable");
    CHECK(sessionListMessage(true, "Connected", &up, 0) == "No sessions - press n");
    CHECK(sessionListMessage(true, "Connected", nullptr, 0) == "No sessions - press n");
    CHECK(sessionListMessage(true, "Connected", &up, 2).empty());
    CHECK(sessionListMessage(true, "Connected", nullptr, 2).empty());
}

TEST_CASE("card header and run state words") {
    CHECK(cardHeader("Hermes", "fix auth", 3, 12) == "Hermes • fix auth • 3/12");
    CHECK(cardHeader("OpenCode", "", 0, 0) == "OpenCode • 0/0");
    CHECK(std::string(runStateWord('i')) == "idle");
    CHECK(std::string(runStateWord('r')) == "running");
    CHECK(std::string(runStateWord('w')) == "needs you");
}

TEST_CASE("run state follows the newest card") {
    int last = 0;
    CHECK(nextRunState(mk(1, CardKind::User), 'i', last) == 'r');
    CHECK(last == 1);
    CHECK(nextRunState(mk(2, CardKind::Approval), 'r', last) == 'w');
    CHECK(nextRunState(mk(2, CardKind::Approval, true), 'w', last) == 'r');
    CHECK(nextRunState(mk(3, CardKind::Question), 'r', last) == 'w');
    CHECK(nextRunState(mk(4, CardKind::Done), 'w', last) == 'i');
    // a late resolved update for an older card does not override done
    CHECK(nextRunState(mk(2, CardKind::Approval, true), 'i', last) == 'i');
    // gap card changes nothing
    CHECK(nextRunState(mk(0, CardKind::Step), 'i', last) == 'i');
    CHECK(nextRunState(mk(5, CardKind::Error), 'r', last) == 'i');
    CHECK(nextRunState(mk(6, CardKind::Step), 'i', last) == 'r');
    CHECK(nextRunState(mk(7, CardKind::Text), 'i', last) == 'r');
    CHECK(last == 7);
}

TEST_CASE("card body text") {
    Card c = mk(1, CardKind::Approval);
    c.title = "Permission: bash";
    c.body = "npm test -- auth";
    CHECK(cardBodyText(c) == "Permission: bash\nnpm test -- auth");
    c.truncated = true;
    CHECK(cardBodyText(c) == "Permission: bash\nnpm test -- auth\n(truncated)");

    Card q = question(2);
    q.title = "Which?";
    CHECK(cardBodyText(q) == "Which?\n1. opt1\n2. opt2");

    Card t = mk(2, CardKind::Text);
    t.body = "hello";
    CHECK(cardBodyText(t) == "hello");
}

TEST_CASE("footer hints per card kind") {
    CHECK(footerHint(nullptr) == "[P] prompt");
    Card yan = approval({ 'Y', 'A', 'N' });
    CHECK(footerHint(&yan) == "[Y] allow [A] always [N] deny");
    Card yn = approval({ 'Y', 'N' });
    CHECK(footerHint(&yn) == "[Y] allow [N] deny");
    Card resolved = approval({ 'Y', 'A', 'N' }, true);
    CHECK(footerHint(&resolved) == "resolved");
    Card q3 = question(3);
    CHECK(footerHint(&q3) == "[1-3] pick [T] type");
    Card q1 = question(1);
    CHECK(footerHint(&q1) == "[1] pick [T] type");
    Card q0 = question(0);
    CHECK(footerHint(&q0) == "[T] type answer");
    Card qr = question(2, true);
    CHECK(footerHint(&qr) == "resolved");
    Card text = mk(1, CardKind::Text);
    CHECK(footerHint(&text) == "[P] prompt [S] stop");
}

TEST_CASE("card banner priority") {
    CHECK(cardBanner(true, "Connected", "", false, 0).empty());
    CHECK(cardBanner(false, "Bridge offline, retrying", "", false, 0) == "Bridge offline, retrying");
    CHECK(cardBanner(false, "Bridge offline, retrying", "card too large", false, 0) == "card too large");
    CHECK(cardBanner(true, "Connected", "", true, 4) == "4 new - Fn+. at end to jump");
    CHECK(cardBanner(true, "Connected", "", true, 0) == "Fn+. at end to jump to latest");
    CHECK(cardBanner(false, "No network", "", true, 4) == "No network");
}

TEST_CASE("card keys: navigation, prompt, stop") {
    Card text = mk(1, CardKind::Text);
    CHECK(cardKeyAction(LV_KEY_PREV, &text, true).action == CardAction::Prev);
    CHECK(cardKeyAction(LV_KEY_NEXT, &text, true).action == CardAction::Next);
    CHECK(cardKeyAction(LV_KEY_ESC, &text, true).action == CardAction::Back);
    CHECK(cardKeyAction(LV_KEY_LEFT, &text, true).action == CardAction::None);
    CHECK(cardKeyAction(LV_KEY_RIGHT, &text, true).action == CardAction::None);
    CHECK(cardKeyAction('p', &text, false).action == CardAction::Prompt);
    CHECK(cardKeyAction('P', &text, false).action == CardAction::Prompt);
    CHECK(cardKeyAction(LV_KEY_ENTER, &text, true).action == CardAction::Prompt);
    CHECK(cardKeyAction(LV_KEY_ENTER, &text, false).action == CardAction::None);
    CHECK(cardKeyAction(LV_KEY_ENTER, nullptr, true).action == CardAction::Prompt);
    CHECK(cardKeyAction('s', &text, false).action == CardAction::Stop);
    CHECK(cardKeyAction('S', nullptr, false).action == CardAction::Stop);
    CHECK(cardKeyAction('y', &text, true).action == CardAction::None);
    CHECK(cardKeyAction('t', &text, true).action == CardAction::None);
    CHECK(cardKeyAction('1', &text, true).action == CardAction::None);
}

TEST_CASE("card keys: approval") {
    Card a = approval({ 'Y', 'A', 'N' });
    auto k = cardKeyAction('y', &a, true);
    CHECK(k.action == CardAction::Respond);
    CHECK(k.choice == "Y");
    CHECK(cardKeyAction('A', &a, true).choice == "A");
    CHECK(cardKeyAction('n', &a, false).choice == "N");

    Card yn = approval({ 'Y', 'N' });
    CHECK(cardKeyAction('a', &yn, true).action == CardAction::None);

    Card resolved = approval({ 'Y', 'A', 'N' }, true);
    CHECK(cardKeyAction('y', &resolved, true).action == CardAction::None);
    CHECK(cardKeyAction('n', &resolved, true).action == CardAction::None);
    CHECK(cardKeyAction('p', &resolved, true).action == CardAction::Prompt);
}

TEST_CASE("card keys: question") {
    Card q = question(3);
    auto k = cardKeyAction('2', &q, true);
    CHECK(k.action == CardAction::Respond);
    CHECK(k.choice == "2");
    CHECK(cardKeyAction('4', &q, true).action == CardAction::None);
    CHECK(cardKeyAction('0', &q, true).action == CardAction::None);
    CHECK(cardKeyAction('t', &q, true).action == CardAction::TypeAnswer);
    CHECK(cardKeyAction('T', &q, true).action == CardAction::TypeAnswer);
    CHECK(cardKeyAction('y', &q, true).action == CardAction::None);

    Card resolved = question(3, true);
    CHECK(cardKeyAction('1', &resolved, true).action == CardAction::None);
    CHECK(cardKeyAction('t', &resolved, true).action == CardAction::None);
}

TEST_CASE("list keys") {
    CHECK(listKeyAction(LV_KEY_PREV) == ListAction::Prev);
    CHECK(listKeyAction(LV_KEY_NEXT) == ListAction::Next);
    CHECK(listKeyAction(LV_KEY_ENTER) == ListAction::Open);
    CHECK(listKeyAction(LV_KEY_ESC) == ListAction::Back);
    CHECK(listKeyAction('n') == ListAction::NewSession);
    CHECK(listKeyAction('N') == ListAction::NewSession);
    CHECK(listKeyAction('m') == ListAction::PickModel);
    CHECK(listKeyAction('M') == ListAction::PickModel);
    CHECK(listKeyAction('x') == ListAction::None);
}

TEST_CASE("card key repeat guard") {
    CHECK(cardKeyBlocked(CardAction::Prompt, true, false));
    CHECK(cardKeyBlocked(CardAction::TypeAnswer, true, false));
    CHECK(cardKeyBlocked(CardAction::Stop, true, false));
    CHECK_FALSE(cardKeyBlocked(CardAction::Prompt, false, true));
    CHECK(cardKeyBlocked(CardAction::Respond, false, true));
    CHECK_FALSE(cardKeyBlocked(CardAction::Respond, true, false));
    CHECK_FALSE(cardKeyBlocked(CardAction::Next, true, true));
    CHECK_FALSE(cardKeyBlocked(CardAction::Prev, true, true));
    CHECK_FALSE(cardKeyBlocked(CardAction::Back, true, true));
}

TEST_CASE("older fetch outcome") {
    CHECK(classifyOlder(std::nullopt) == OlderResult::Failed);
    CHECK(classifyOlder(std::vector<Card>()) == OlderResult::StartReached);
    CHECK(classifyOlder(std::vector<Card> { mk(3, CardKind::Text) }) == OlderResult::Prepend);
}

TEST_CASE("low memory thresholds") {
    CHECK_FALSE(isLowMemory(80 * 1024, 40 * 1024));
    CHECK_FALSE(isLowMemory(30 * 1024, 16 * 1024));
    CHECK(isLowMemory(30 * 1024 - 1, 40 * 1024));
    CHECK(isLowMemory(80 * 1024, 16 * 1024 - 1));
}
