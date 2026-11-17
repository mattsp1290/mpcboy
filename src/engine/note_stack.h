// src/engine/note_stack.h - held-note stack, last-note priority, no allocation.
#pragma once

#include <cstdint>

namespace mpcboy {

class NoteStack
{
public:
    static constexpr int kCapacity = 16;

    void clear() { n_ = 0; }
    bool empty() const { return n_ == 0; }
    int size() const { return n_; }
    // Top (most recent) note; only valid when !empty().
    int top() const { return notes_[n_ - 1]; }

    // Push a note (moves it to the top if already held). A 17th distinct
    // note replaces the oldest entry.
    void push(int note)
    {
        remove(note);
        if (n_ == kCapacity)
        {
            for (int i = 1; i < kCapacity; ++i) notes_[i - 1] = notes_[i];
            --n_;
        }
        notes_[n_++] = static_cast<uint8_t>(note);
    }

    // Returns true when the note was held.
    bool remove(int note)
    {
        for (int i = 0; i < n_; ++i)
        {
            if (notes_[i] == note)
            {
                for (int j = i + 1; j < n_; ++j) notes_[j - 1] = notes_[j];
                --n_;
                return true;
            }
        }
        return false;
    }

private:
    uint8_t notes_[kCapacity] = {};
    int n_ = 0;
};

}  // namespace mpcboy
