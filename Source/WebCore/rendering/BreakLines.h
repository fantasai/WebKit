/*
 * Copyright (C) 2005-2023 Apple Inc. All rights reserved.
 * Copyright (C) 2011 Google Inc. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#pragma once

#include <unicode/ubrk.h>
#include <wtf/ASCIICType.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/TextBreakIterator.h>
#include <wtf/unicode/CharacterNames.h>

namespace WebCore {

class BreakLines {
public:
    enum class NoBreakSpaceBehavior {
        Normal,
        Break,
    };
    enum class WordBreakBehavior {
        Normal,
        BreakAll,
        KeepAll,
    };
    enum class LineBreakRules {
        Normal, // Fast path available when using default line-breaking rules within ASCII.
        Special, // Uses ICU to handle special line-breaking rules.
    };
    template<LineBreakRules, WordBreakBehavior, NoBreakSpaceBehavior>
    static inline unsigned nextBreakablePosition(CachedLineBreakIteratorFactory&, size_t startPosition);

    static inline unsigned nextBreakablePosition(CachedLineBreakIteratorFactory& iterator, size_t startPosition)
    {
        return nextBreakablePosition<LineBreakRules::Normal, WordBreakBehavior::Normal, NoBreakSpaceBehavior::Normal>(iterator, startPosition);
    }

    static inline bool isBreakable(CachedLineBreakIteratorFactory&, unsigned startPosition, std::optional<unsigned>& nextBreakable, bool breakNBSP, bool canUseShortcut, bool keepAllWords, bool breakAnywhere);

private:

    // Iterator implementations.
    template<typename CharacterType, LineBreakRules, NoBreakSpaceBehavior>
    static inline size_t nextBreakablePosition(CachedLineBreakIteratorFactory&, std::span<const CharacterType> string, size_t startPosition);

    template<typename CharacterType, NoBreakSpaceBehavior>
    static inline size_t nextBreakableSpace(std::span<const CharacterType> string, size_t startPosition);

    static inline unsigned nextCharacter(CachedLineBreakIteratorFactory&, unsigned startPosition);

    // Helper functions.
    enum BreakClass : int;
    template<NoBreakSpaceBehavior>
    static BreakClass classify(UChar character);

    enum class BreakResult {
        Defer, // Defer to ICU
        Break,
        Keep,
        Rewind,
    };
    template<WordBreakBehavior>
    static inline BreakResult isBreakable(BreakClass before, BreakClass after);

    template<NoBreakSpaceBehavior>
    static inline bool isBreakableSpace(UChar character);

    // Data types.
    enum BreakClass : int {
        // See UAX14
        Indeterminate = 0,
        AL = 1,
        ID = 1 << 1,
        CM = 1 << 2,
        OP = 1 << 3,
        CP = 1 << 4,
        CL = 1 << 5,
        GL = 1 << 6,
        NU = 1 << 7,
        Weird = 1 << 15,
        // Currently we map HL to AL and H2 and H3 to ID.
        // If we pull more logic into isBreakable, these may need to be distinguished.
    };

    template<typename CharacterType>
    struct CharacterInfo {
        CharacterType id;
        BreakClass type;
        CharacterInfo(CharacterType character = 0):id(character), type(Indeterminate) {}
        inline void set(CharacterType character)
        {
            id = character;
            type = Indeterminate;
        }
        operator CharacterType() const { return id; }
    };

    class LineBreakTable {
    public:
        static constexpr UChar firstCharacter = '!';
        static constexpr UChar lastCharacter = 127;
        static inline bool unsafeLookup(UChar before, UChar after) // Must range check before calling.
        {
            const unsigned beforeIndex = before - firstCharacter;
            const unsigned afterIndex = after - firstCharacter;
            return breakTable[beforeIndex][afterIndex / 8] & (1 << (afterIndex % 8));
        }
    private:
        static constexpr unsigned rowCount = lastCharacter - firstCharacter + 1;
        static constexpr unsigned columnCount = (lastCharacter - firstCharacter) / 8 + 1;
        WEBCORE_EXPORT static const unsigned char breakTable[rowCount][columnCount];
    };
    static const LineBreakTable lineBreakTable;
};


template<BreakLines::WordBreakBehavior words>
inline BreakLines::BreakResult BreakLines::isBreakable(BreakClass before, BreakClass after)
{
    /* Short-circuit the commonest cases: letter + letter. */
    int pair = before | after;
    if (pair == (AL | AL))
        return (words == WordBreakBehavior::BreakAll)
            ? BreakResult::Break
            : BreakResult::Keep;
    if ((pair | AL) == (ID | AL))
        return (words == WordBreakBehavior::KeepAll)
            ? BreakResult::Keep
            : BreakResult::Break;

    /* Handle special cases. */
    if (pair & GL && !(pair & Weird)) // Keep nbsp high in our list.
        return BreakResult::Keep;
    if (after == CM)
        return BreakResult::Keep;
    if (pair & Weird)
        return BreakResult::Defer;

    /* Switch the remainder. */
#define pair(before, after) (before << sizeof(char)) | after
    switch (pair(before, after)) {
    case pair(CM, AL):
    case pair(CM, ID):
        return BreakResult::Rewind;
    }
#undef pair

    return BreakResult::Defer;
}

template<BreakLines::NoBreakSpaceBehavior nonBreakingSpaceBehavior>
inline bool BreakLines::isBreakableSpace(UChar character)
{
    switch (character) {
    case ' ':
    case '\n':
    case '\t':
        return true;
    case noBreakSpace:
        return nonBreakingSpaceBehavior == NoBreakSpaceBehavior::Break;
    default:
        return false;
    }
}

template<typename CharacterType, BreakLines::LineBreakRules shortcutRules, BreakLines::NoBreakSpaceBehavior nonBreakingSpaceBehavior>
inline size_t BreakLines::nextBreakablePosition(CachedLineBreakIteratorFactory& lineBreakIteratorFactory, std::span<const CharacterType> string, size_t startPosition)
{
    // Don't break if positioned at start of primary context and there is no prior context.
    auto priorContextLength = lineBreakIteratorFactory.priorContext().length();
    if (startPosition == 0 && !priorContextLength)
        startPosition++;

    CharacterInfo<CharacterType> beforeBefore(startPosition > 1 ? string[startPosition - 2]
        : static_cast<CharacterType>(lineBreakIteratorFactory.priorContext().secondToLastCharacter()));
    CharacterInfo<CharacterType> before(startPosition > 0 ? string[startPosition - 1]
        : static_cast<CharacterType>(lineBreakIteratorFactory.priorContext().lastCharacter()));
    CharacterInfo<CharacterType> after;

    std::optional<size_t> nextBreak;
    for (size_t i = startPosition; i < string.size(); beforeBefore = before, before = after, ++i) {
        after.set(string[i]);

        // Breakable spaces.
        if (isBreakableSpace<nonBreakingSpaceBehavior>(after))
            return i;

        // ASCII rapid lookup.
        if (shortcutRules == LineBreakRules::Normal) { // Not valid for 'loose' line-breaking.

            // Don't allow line breaking between '-' and a digit if the '-' may mean a minus sign in the context,
            // while allow breaking in 'ABCD-1234' and '1234-5678' which may be in long URLs.
            if (before == '-' && isASCIIDigit(after)) {
                if (isASCIIAlphanumeric(beforeBefore))
                    return i;
                continue;
            }

            // If both characters are ASCII, use a lookup table for enhanced speed
            // and for compatibility with other browsers (see comments on lineBreakTable for details).
            if (before <= lineBreakTable.lastCharacter && after <= lineBreakTable.lastCharacter) {
                if (before >= lineBreakTable.firstCharacter && after >= lineBreakTable.firstCharacter) {
                    if (lineBreakTable.unsafeLookup(before, after))
                        return i;
                } // Else at least one is an ASCII control character; don't break.
                continue;
            }
        }

        // Non-ASCII rapid lookup.
        if (!before.type)
            before.type = classify<nonBreakingSpaceBehavior>(before);
        after.type = classify<nonBreakingSpaceBehavior>(after);
        switch (isBreakable<BreakLines::WordBreakBehavior::Normal>(before.type, after.type)) {
        case BreakResult::Break:
            return i;
        case BreakResult::Keep:
            continue;
        default:
            break; // Fall through.
        }

        // ICU lookup (slow).
        if (!nextBreak || nextBreak.value() < i) {
            auto& breakIterator = lineBreakIteratorFactory.get();
            nextBreak = breakIterator.following(i - 1);
        }
        // Fast forward while our behavior matches ICU.
        if (i != nextBreak) {
            for (size_t max = std::min(nextBreak.value(), string.size() - 1); i < max; beforeBefore = before, before = after, ++i) {
                CharacterType lookahead = string[i + 1];
                if (lookahead <= lineBreakTable.lastCharacter
                    || (nonBreakingSpaceBehavior == NoBreakSpaceBehavior::Break && lookahead == noBreakSpace))
                    break;
            }
        }
        if (i == nextBreak && !isBreakableSpace<nonBreakingSpaceBehavior>(before))
            return i;
    }

    return string.size();
}

template<typename CharacterType, BreakLines::NoBreakSpaceBehavior nonBreakingSpaceBehavior>
inline size_t BreakLines::nextBreakableSpace(std::span<const CharacterType> string, size_t startPosition)
{
    // FIXME: Use ICU instead.
    for (size_t i = startPosition; i < string.size(); ++i) {
        if (isBreakableSpace<nonBreakingSpaceBehavior>(string[i]))
            return i;
        // FIXME: This should either be in isBreakableSpace (though previous attempts broke the world) or should use ICU instead.
        if (string[i] == zeroWidthSpace)
            return i;
        if (string[i] == ideographicSpace)
            return i + 1;
    }
    return string.size();
}

inline unsigned BreakLines::nextCharacter(CachedLineBreakIteratorFactory& lineBreakIteratorFactory, unsigned startPosition)
{
    auto stringView = lineBreakIteratorFactory.stringView();
    ASSERT(startPosition <= stringView.length());
    // FIXME: Can/Should we implement this using a Shared Iterator (performance issue)
    // https://bugs.webkit.org/show_bug.cgi?id=197876
    NonSharedCharacterBreakIterator iterator(stringView);
    std::optional<unsigned> next = ubrk_following(iterator, startPosition);
    return next.value_or(stringView.length());
}

template<BreakLines::LineBreakRules rules, BreakLines::WordBreakBehavior words, BreakLines::NoBreakSpaceBehavior spaces>
inline unsigned BreakLines::nextBreakablePosition(CachedLineBreakIteratorFactory& lineBreakIteratorFactory, size_t startPosition)
{
    auto stringView = lineBreakIteratorFactory.stringView();
    if (stringView.is8Bit()) {
        return words == WordBreakBehavior::KeepAll
            ? nextBreakableSpace<LChar, spaces>(stringView.span8(), startPosition)
            : nextBreakablePosition<LChar, rules, spaces>(lineBreakIteratorFactory, stringView.span8(), startPosition);
    }
    return words == WordBreakBehavior::KeepAll
        ? nextBreakableSpace<UChar, spaces>(stringView.span16(), startPosition)
        : nextBreakablePosition<UChar, rules, spaces>(lineBreakIteratorFactory, stringView.span16(), startPosition);
}


inline bool BreakLines::isBreakable(CachedLineBreakIteratorFactory& lineBreakIteratorFactory, unsigned startPosition, std::optional<unsigned>& nextBreakable, bool breakNBSP, bool canUseShortcut, bool keepAllWords, bool breakAnywhere)
{
    if (nextBreakable && nextBreakable.value() >= startPosition)
        return startPosition == nextBreakable;

    if (breakAnywhere)
        return startPosition == nextCharacter(lineBreakIteratorFactory, startPosition);

    if (keepAllWords) {
        if (breakNBSP)
            return startPosition == nextBreakablePosition<LineBreakRules::Special, WordBreakBehavior::KeepAll, NoBreakSpaceBehavior::Break>(lineBreakIteratorFactory, startPosition);
        return startPosition == nextBreakablePosition<LineBreakRules::Special, WordBreakBehavior::KeepAll, NoBreakSpaceBehavior::Normal>(lineBreakIteratorFactory, startPosition);
    }

    if (canUseShortcut) {
        if (breakNBSP)
            return startPosition == nextBreakablePosition<LineBreakRules::Normal, WordBreakBehavior::Normal, NoBreakSpaceBehavior::Break>(lineBreakIteratorFactory, startPosition);
        return startPosition == nextBreakablePosition<LineBreakRules::Normal, WordBreakBehavior::Normal, NoBreakSpaceBehavior::Normal>(lineBreakIteratorFactory, startPosition);
    }

    if (breakNBSP)
        return startPosition == nextBreakablePosition<LineBreakRules::Special, WordBreakBehavior::Normal, NoBreakSpaceBehavior::Break>(lineBreakIteratorFactory, startPosition);
    return startPosition == nextBreakablePosition<LineBreakRules::Special, WordBreakBehavior::Normal, NoBreakSpaceBehavior::Normal>(lineBreakIteratorFactory, startPosition);
}

template<BreakLines::NoBreakSpaceBehavior nonBreakingSpaceBehavior>
BreakLines::BreakClass BreakLines::classify(UChar character)
{
    const UChar blockLast3 = ~0x07;
    // const UChar blockLast4 = ~0x0F;
    const UChar blockLast6 = ~0x3F;
    const UChar blockLast7 = ~0x7F;
    const UChar blockLast8 = ~0xFF;

    switch (character & blockLast7) {
    case 0x0000: // ASCII
        if ((0x005E <= character && character <= 0x007A) // Check lowercase first.
            || (0x0040 <= character && character <= 0x005A) )
            return AL;
        return Weird;
    case 0x0080: // Latin-1
        if (nonBreakingSpaceBehavior == NoBreakSpaceBehavior::Normal && character == 0xA0)
            return GL;
        if (character > 0x00C0)
            return AL;
        return Weird;
    case 0x0100:
    case 0x0180:
    case 0x0200:
        return AL;
    case 0x0280:
        switch (character) {
        case 0x02C8:
        case 0x02CC:
        case 0x02DF:
            return Weird;
        default:
            return AL;
        }
    case 0x0300:
        if (character == 0x034F || (0x035C <= character && character <= 0x0362))
            return GL;
        if (character < 0x370)
            return CM;
        if (UNLIKELY(character == 0x037E))
            return Weird;
        return AL;
    case 0x0380:
    case 0x0400:
        return AL;
    case 0x0480:
        if (0x0483 <= character && character <= 0x0489)
            return CM;
        return AL;
    case 0x0500:
        return AL;
    case 0x0580:
        if (character <= 0x0588 || 0x05C8 <= character)
            return AL; // WARNING: Some of these are actually HL.
        if (0x0591 <= character && character <= 0x05BD)
            return CM;
        // 0x05BE to 0x05C7 is mixed up.
        switch (character) {
        case 0x05BE:
        case 0x05C6:
            return Weird;
        case 0x05C0:
        case 0x05C3:
            return AL;
        default:
            return CM;
        }
    // Continue bitmask switch up to 2E80.
    }

    if (0x2E80 <= character && character <= 0xA4CF) { // CJK
        if ((character & blockLast8) == 0x3000) {
            // Fun stuff, make a table.
            return Weird;
        }
        // Insert CJ logic here.
        if (UNLIKELY((character & blockLast3) == 0x3248))
            return AL;
        if (UNLIKELY((character & blockLast6) == 0x4DC0))
            return AL;
        if (UNLIKELY(character == 0xA015))
            return Weird;
        return ID;
    }
    if (0xAC00 <= character && character <= 0xD7AF) // Precomposed Hangul
        return ID; // WARNING: These are actually H2 or H3.
    if (0xF900 <= character && character <= 0XFAFF) // More CJK
        return ID;

    return Weird;
}

} // namespace WebCore
