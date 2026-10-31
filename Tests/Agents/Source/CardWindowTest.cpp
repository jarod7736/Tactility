#include <doctest.h>

#include <agents/CardWindow.h>

using namespace agents;

namespace {
Card mk(int seq, bool resolved = false) {
    Card c;
    c.seq = seq;
    c.title = "t" + std::to_string(seq);
    c.resolved = resolved;
    return c;
}
std::vector<Card> range(int from, int to) {
    std::vector<Card> v;
    for (int i = from; i <= to; i++) v.push_back(mk(i));
    return v;
}
} // namespace

TEST_CASE("empty window") {
    CardWindow w;
    CHECK(w.current() == nullptr);
    CHECK(w.position() == 0);
    CHECK(w.size() == 0);
    CHECK(w.oldestSeq() == -1);
    CHECK(w.lastSeq() == -1);
    CHECK_FALSE(w.next());
    CHECK_FALSE(w.prev());
}

TEST_CASE("first card on empty window is Appended, even with low seq") {
    CardWindow w;
    CHECK(w.apply(mk(1)) == CardWindow::Change::Appended);
    CHECK(w.size() == 1);
    CHECK(w.current()->seq == 1);
    CHECK(w.position() == 1);
}

TEST_CASE("capacity 20 drops oldest") {
    CardWindow w(20);
    for (int i = 1; i <= 25; i++) w.apply(mk(i));
    CHECK(w.size() == 20);
    CHECK(w.oldestSeq() == 6);
    CHECK(w.lastSeq() == 25);
}

TEST_CASE("following latest moves current on append") {
    CardWindow w;
    w.apply(mk(1));
    w.apply(mk(2));
    CHECK(w.followingLatest());
    w.apply(mk(3));
    CHECK(w.current()->seq == 3);
    CHECK(w.position() == 3);
}

TEST_CASE("not following latest keeps current on append") {
    CardWindow w;
    for (int i = 1; i <= 3; i++) w.apply(mk(i));
    CHECK(w.prev());
    CHECK_FALSE(w.followingLatest());
    w.apply(mk(4));
    CHECK(w.current()->seq == 2);
    CHECK_FALSE(w.followingLatest());
    CHECK(w.size() == 4);
}

TEST_CASE("prev/next boundaries") {
    CardWindow w;
    for (int i = 1; i <= 3; i++) w.apply(mk(i));
    CHECK_FALSE(w.next());
    CHECK(w.prev());
    CHECK(w.prev());
    CHECK(w.current()->seq == 1);
    CHECK_FALSE(w.prev());
    CHECK(w.current()->seq == 1);
    CHECK(w.next());
    CHECK(w.current()->seq == 2);
}

TEST_CASE("existing seq updates in place") {
    CardWindow w;
    for (int i = 1; i <= 3; i++) w.apply(mk(i));
    CHECK(w.prev());
    CHECK(w.apply(mk(2, true)) == CardWindow::Change::Updated);
    CHECK(w.size() == 3);
    CHECK(w.current()->seq == 2);
    CHECK(w.current()->resolved);
}

TEST_CASE("update of an older card keeps current") {
    CardWindow w;
    for (int i = 1; i <= 3; i++) w.apply(mk(i));
    CHECK(w.apply(mk(3, true)) == CardWindow::Change::Updated);
    CHECK(w.current()->resolved);
    CHECK(w.followingLatest());
}

TEST_CASE("lower seq not present means bridge restart") {
    CardWindow w;
    for (int i = 40; i <= 50; i++) w.apply(mk(i));
    CHECK(w.apply(mk(1)) == CardWindow::Change::Reset);
    CHECK(w.size() == 1);
    CHECK(w.current()->seq == 1);
    CHECK(w.position() == 1);
    CHECK(w.followingLatest());
    CHECK(w.apply(mk(2)) == CardWindow::Change::Appended);
    CHECK(w.current()->seq == 2);
}

TEST_CASE("trim clamps current to new oldest") {
    CardWindow w(3);
    for (int i = 1; i <= 3; i++) w.apply(mk(i));
    CHECK(w.prev());
    CHECK(w.prev());
    CHECK(w.current()->seq == 1);
    w.apply(mk(4));  // drops seq 1
    CHECK(w.oldestSeq() == 2);
    CHECK(w.current()->seq == 2);
    CHECK(w.position() == 1);
}

TEST_CASE("trim keeps current on the same card when it survives") {
    CardWindow w(3);
    for (int i = 1; i <= 3; i++) w.apply(mk(i));
    CHECK(w.prev());  // seq 2
    w.apply(mk(4));
    CHECK(w.current()->seq == 2);
    CHECK(w.position() == 1);
}

TEST_CASE("prependOlder inserts before oldest, current stays on same card") {
    CardWindow w;
    for (int i = 10; i <= 12; i++) w.apply(mk(i));
    CHECK(w.prev());
    CHECK(w.current()->seq == 11);
    w.prependOlder(range(7, 9));
    CHECK(w.size() == 6);
    CHECK(w.oldestSeq() == 7);
    CHECK(w.current()->seq == 11);
    CHECK(w.position() == 5);
}

TEST_CASE("prependOlder ignores seqs at or above oldest") {
    CardWindow w;
    for (int i = 10; i <= 12; i++) w.apply(mk(i));
    w.prependOlder(range(8, 11));
    CHECK(w.size() == 5);
    CHECK(w.oldestSeq() == 8);
    CHECK(w.lastSeq() == 12);
    CHECK(w.current()->seq == 12);
}

TEST_CASE("prependOlder with nothing usable is a no-op") {
    CardWindow w;
    for (int i = 10; i <= 12; i++) w.apply(mk(i));
    w.prependOlder({});
    w.prependOlder(range(10, 15));
    CHECK(w.size() == 3);
    CHECK(w.position() == 3);
}

TEST_CASE("prependOlder at capacity drops newest cards after current") {
    CardWindow w(5);
    for (int i = 10; i <= 14; i++) w.apply(mk(i));
    for (int i = 0; i < 4; i++) CHECK(w.prev());  // current = 10
    w.prependOlder(range(8, 9));
    CHECK(w.size() == 5);
    CHECK(w.oldestSeq() == 8);
    CHECK(w.lastSeq() == 12);  // 13, 14 dropped
    CHECK(w.current()->seq == 10);
    CHECK(w.position() == 3);
}

TEST_CASE("prependOlder never drops current: truncates prepended set instead") {
    CardWindow w(5);
    for (int i = 10; i <= 14; i++) w.apply(mk(i));
    // current = 14 (latest): nothing after it to drop
    w.prependOlder(range(1, 9));
    CHECK(w.size() == 5);
    CHECK(w.lastSeq() == 14);
    CHECK(w.oldestSeq() == 10);
    CHECK(w.current()->seq == 14);
}

TEST_CASE("prependOlder partial: drops newest then truncates prepended") {
    CardWindow w(5);
    for (int i = 10; i <= 14; i++) w.apply(mk(i));
    CHECK(w.prev());
    CHECK(w.prev());  // current = 12, two after it
    w.prependOlder(range(1, 9));  // excess 9, droppable 2
    CHECK(w.size() == 5);
    CHECK(w.lastSeq() == 12);
    CHECK(w.oldestSeq() == 8);  // 2 newest of the older set fit once 13, 14 are gone
    CHECK(w.current()->seq == 12);
    CHECK(w.position() == 5);
}

TEST_CASE("setCapacity shrinks by dropping oldest") {
    CardWindow w(20);
    for (int i = 1; i <= 20; i++) w.apply(mk(i));
    w.setCapacity(10);
    CHECK(w.size() == 10);
    CHECK(w.oldestSeq() == 11);
    CHECK(w.current()->seq == 20);
}

TEST_CASE("setCapacity clamps current to new oldest") {
    CardWindow w(20);
    for (int i = 1; i <= 20; i++) w.apply(mk(i));
    for (int i = 0; i < 19; i++) CHECK(w.prev());
    w.setCapacity(10);
    CHECK(w.current()->seq == 11);
    CHECK(w.position() == 1);
}

TEST_CASE("setCapacity larger keeps cards") {
    CardWindow w(5);
    for (int i = 1; i <= 5; i++) w.apply(mk(i));
    w.setCapacity(10);
    for (int i = 6; i <= 12; i++) w.apply(mk(i));
    CHECK(w.size() == 10);
    CHECK(w.oldestSeq() == 3);
}

TEST_CASE("prependOlder on empty window puts cursor on newest prepended card") {
    CardWindow w;
    w.prependOlder(range(1, 3));
    CHECK(w.size() == 3);
    CHECK(w.current()->seq == 3);
    CHECK(w.position() == 3);
    CHECK(w.followingLatest());
    CHECK_FALSE(w.detached());
}

TEST_CASE("prependOlder dropping newest cards detaches the window") {
    CardWindow w(5);
    for (int i = 10; i <= 14; i++) w.apply(mk(i));
    for (int i = 0; i < 4; i++) CHECK(w.prev());
    w.prependOlder(range(8, 9));
    CHECK(w.detached());
    CHECK_FALSE(w.followingLatest());
    CHECK(w.pendingNew() == 0);
}

TEST_CASE("prependOlder without dropping newest does not detach") {
    CardWindow w;
    for (int i = 10; i <= 12; i++) w.apply(mk(i));
    w.prependOlder(range(7, 9));
    CHECK_FALSE(w.detached());
}

TEST_CASE("detached window ignores new seqs and counts them, still updates") {
    CardWindow w(5);
    for (int i = 10; i <= 14; i++) w.apply(mk(i));
    for (int i = 0; i < 4; i++) CHECK(w.prev());
    w.prependOlder(range(8, 9));  // lastSeq now 12
    CHECK(w.apply(mk(15)) == CardWindow::Change::Ignored);
    CHECK(w.apply(mk(16)) == CardWindow::Change::Ignored);
    CHECK(w.pendingNew() == 2);
    CHECK(w.size() == 5);
    CHECK(w.lastSeq() == 12);
    CHECK(w.apply(mk(11, true)) == CardWindow::Change::Updated);
    CHECK(w.pendingNew() == 2);
    // even at the last card, a detached window is not following latest
    while (w.next()) {}
    CHECK_FALSE(w.followingLatest());
}

TEST_CASE("clear resets detached state") {
    CardWindow w(5);
    for (int i = 10; i <= 14; i++) w.apply(mk(i));
    for (int i = 0; i < 4; i++) CHECK(w.prev());
    w.prependOlder(range(8, 9));
    w.apply(mk(15));
    w.clear();
    CHECK(w.size() == 0);
    CHECK(w.current() == nullptr);
    CHECK(w.position() == 0);
    CHECK_FALSE(w.detached());
    CHECK(w.pendingNew() == 0);
    CHECK(w.apply(mk(1)) == CardWindow::Change::Appended);
}

TEST_CASE("Reset clears detached state") {
    CardWindow w(5);
    for (int i = 10; i <= 14; i++) w.apply(mk(i));
    for (int i = 0; i < 4; i++) CHECK(w.prev());
    w.prependOlder(range(8, 9));
    w.apply(mk(15));
    CHECK(w.apply(mk(1)) == CardWindow::Change::Reset);
    CHECK_FALSE(w.detached());
    CHECK(w.pendingNew() == 0);
    CHECK(w.apply(mk(2)) == CardWindow::Change::Appended);
}

TEST_CASE("stale resolved update below oldest is ignored") {
    CardWindow w;
    for (int i = 10; i <= 12; i++) w.apply(mk(i));
    CHECK(w.apply(mk(5, true)) == CardWindow::Change::Ignored);
    CHECK(w.size() == 3);
    CHECK(w.oldestSeq() == 10);
}

TEST_CASE("unresolved seq below oldest is a bridge restart") {
    CardWindow w;
    for (int i = 10; i <= 12; i++) w.apply(mk(i));
    CHECK(w.apply(mk(5)) == CardWindow::Change::Reset);
    CHECK(w.size() == 1);
}

TEST_CASE("missing seq inside the window is ignored") {
    CardWindow w;
    w.apply(mk(10));
    w.apply(mk(12));
    CHECK(w.apply(mk(11)) == CardWindow::Change::Ignored);
    CHECK(w.size() == 2);
}
