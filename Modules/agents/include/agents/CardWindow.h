#pragma once

#include "agents/Card.h"

#include <cstddef>
#include <deque>
#include <vector>

namespace agents {

/** The last N cards shown on the card screen, ordered oldest to newest, plus a cursor ("current").
 *  Hardware-free and exception-free. All seqs are assumed ascending within the window. */
class CardWindow {
public:
    enum class Change { Appended, Updated, Reset };

    /** A capacity of 0 is treated as 1. */
    explicit CardWindow(size_t capacity = 20);

    /** - seq already present: replace that card in place (Updated); the cursor does not move.
     *  - seq > lastSeq (or window empty): append (Appended). The cursor moves to the new card only if it
     *    was on the latest card. Over capacity, the oldest cards are dropped; a cursor on a dropped card
     *    is clamped to the new oldest.
     *  - seq < lastSeq and not present: bridge restarted. Clear, append, cursor on it (Reset). */
    Change apply(const Card& card);

    /** Insert older cards before the oldest. Input is oldest first. Cards with seq >= oldestSeq() are
     *  ignored. The cursor stays on the same card (its position() shifts).
     *  Capacity rule: the cursor card is never dropped. If the result would exceed capacity, cards
     *  after the cursor (newest end) are dropped first; if that is still not enough, the prepended set
     *  is truncated, keeping its newest cards (those contiguous with the existing oldest). */
    void prependOlder(std::vector<Card> olderOldestFirst);

    /** Card under the cursor, or nullptr when empty. */
    const Card* current() const;
    /** 1-based index of the cursor for header text such as "3/12" (with size()), or 0 when empty. */
    size_t position() const;
    size_t size() const { return cards_.size(); }
    /** Seq of the oldest / newest card, or -1 when empty. */
    int oldestSeq() const;
    int lastSeq() const;

    /** Move toward newer / older. Return false (cursor unchanged) at the edge; a false prev() means
     *  the caller should fetch older cards. */
    bool next();
    bool prev();

    /** True when the cursor is on the latest card (or the window is empty). */
    bool followingLatest() const;

    /** Shrinking drops the oldest cards, clamping the cursor like apply(). 0 is treated as 1. */
    void setCapacity(size_t capacity);

private:
    void trimOldest();

    std::deque<Card> cards_;
    size_t capacity_;
    size_t cursor_ = 0;
};

} // namespace agents
