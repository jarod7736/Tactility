#include "agents/CardWindow.h"

#include <algorithm>
#include <climits>
#include <iterator>

namespace agents {

CardWindow::CardWindow(size_t capacity) : capacity_(std::max<size_t>(capacity, 1)) {}

void CardWindow::trimOldest() {
    if (cards_.size() <= capacity_) return;
    size_t drop = cards_.size() - capacity_;
    cards_.erase(cards_.begin(), cards_.begin() + static_cast<std::ptrdiff_t>(drop));
    cursor_ = cursor_ >= drop ? cursor_ - drop : 0;
}

CardWindow::Change CardWindow::apply(const Card& card) {
    for (auto& existing : cards_) {
        if (existing.seq == card.seq) {
            existing = card;
            return Change::Updated;
        }
    }
    if (cards_.empty()) {
        cards_.push_back(card);
        cursor_ = 0;
        return Change::Appended;
    }
    if (card.seq > cards_.back().seq) {
        if (detached_) {
            pendingNew_++;
            return Change::Ignored;
        }
        bool following = followingLatest();
        cards_.push_back(card);
        if (following) cursor_ = cards_.size() - 1;
        trimOldest();
        return Change::Appended;
    }
    if (card.seq < cards_.front().seq && !card.resolved) {
        cards_.clear();
        cards_.push_back(card);
        cursor_ = 0;
        detached_ = false;
        pendingNew_ = 0;
        return Change::Reset;
    }
    // Stale resolved update for an evicted card, or a gap inside the window.
    return Change::Ignored;
}

void CardWindow::clear() {
    cards_.clear();
    cursor_ = 0;
    detached_ = false;
    pendingNew_ = 0;
}

void CardWindow::prependOlder(std::vector<Card> olderOldestFirst) {
    int limit = cards_.empty() ? INT_MAX : cards_.front().seq;
    std::vector<Card> add;
    for (auto& c : olderOldestFirst) {
        if (c.seq < limit && (add.empty() || c.seq > add.back().seq)) add.push_back(std::move(c));
    }
    if (add.empty()) return;

    bool wasEmpty = cards_.empty();
    size_t total = cards_.size() + add.size();
    if (total > capacity_) {
        size_t excess = total - capacity_;
        size_t afterCursor = cards_.empty() ? 0 : cards_.size() - 1 - cursor_;
        size_t dropNewest = std::min(excess, afterCursor);
        cards_.erase(cards_.end() - static_cast<std::ptrdiff_t>(dropNewest), cards_.end());
        if (dropNewest > 0) detached_ = true;
        excess -= dropNewest;
        if (excess > 0) {
            // Keep the newest of the prepended set; if even that exceeds capacity, only what fits.
            size_t cut = std::min(excess, add.size());
            add.erase(add.begin(), add.begin() + static_cast<std::ptrdiff_t>(cut));
        }
    }
    cursor_ = wasEmpty ? add.size() - 1 : cursor_ + add.size();
    cards_.insert(cards_.begin(), std::make_move_iterator(add.begin()), std::make_move_iterator(add.end()));
}

const Card* CardWindow::current() const {
    return cards_.empty() ? nullptr : &cards_[cursor_];
}

size_t CardWindow::position() const {
    return cards_.empty() ? 0 : cursor_ + 1;
}

int CardWindow::oldestSeq() const {
    return cards_.empty() ? -1 : cards_.front().seq;
}

int CardWindow::lastSeq() const {
    return cards_.empty() ? -1 : cards_.back().seq;
}

bool CardWindow::next() {
    if (cards_.empty() || cursor_ + 1 >= cards_.size()) return false;
    cursor_++;
    return true;
}

bool CardWindow::prev() {
    if (cards_.empty() || cursor_ == 0) return false;
    cursor_--;
    return true;
}

bool CardWindow::followingLatest() const {
    return cards_.empty() || (!detached_ && cursor_ + 1 == cards_.size());
}

void CardWindow::setCapacity(size_t capacity) {
    capacity_ = std::max<size_t>(capacity, 1);
    trimOldest();
}

} // namespace agents
