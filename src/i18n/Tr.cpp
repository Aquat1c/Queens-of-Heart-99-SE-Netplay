#include "Tr.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace qi18n {
namespace {

struct Row {
    const char* en;
    const char* ja;
};

// Must stay sorted by `en` in strcmp order: the lookup is a binary search.
#include "TrTableJa.inc"

Language g_language = Language::English;

bool rowLess(const Row& row, const char* key) {
    return std::strcmp(row.en, key) < 0;
}

bool labelShaped(const char* key) {
    std::size_t length = std::strlen(key);
    while (length > 0u && key[length - 1u] == ' ') --length;
    if (length == 0u) return false;
    switch (key[length - 1u]) {
        case ':': case '(': case '=': case '-':
            return true;
        default:
            return false;
    }
}

const Row* longestLabelPrefix(const char* text) {
    if (text[0] == '\0') return nullptr;
    const Row* const begin = kJapaneseRows;
    const Row* const end = kJapaneseRows + kJapaneseRowCount;
    const Row* row = std::lower_bound(begin, end, text, rowLess);
    while (row != begin) {
        --row;
        if (row->en[0] != text[0]) break;
        const std::size_t length = std::strlen(row->en);
        if (length != 0u && std::strncmp(row->en, text, length) == 0
            && labelShaped(row->en)) {
            return row;
        }
    }
    return nullptr;
}

}

bool setLanguage(Language requested) {
    switch (requested) {
        case Language::English:
        case Language::Japanese:
            g_language = requested;
            return true;
    }
    return false;
}

bool setLanguageFromIniValue(int value) {
    switch (value) {
        case 0: return setLanguage(Language::English);
        case 1: return setLanguage(Language::Japanese);
        default: break;
    }
    return false;
}

Language language() { return g_language; }

const char* languageName(Language which) {
    switch (which) {
        case Language::Japanese: return "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E";
        case Language::English:  break;
    }
    return "English";
}

const char* tr(const char* english) {
    if (english == nullptr) return english;
    if (g_language == Language::English) return english;
    const Row* const begin = kJapaneseRows;
    const Row* const end = kJapaneseRows + kJapaneseRowCount;
    const Row* const found = std::lower_bound(begin, end, english, rowLess);
    if (found == end || std::strcmp(found->en, english) != 0) {
        return english;
    }
    return found->ja;
}

std::string trText(const std::string& text) {
    if (g_language == Language::English || text.empty()) return text;

    std::string out;
    const char* rest = text.c_str();
    for (int segment = 0; segment < 8 && *rest != '\0'; ++segment) {
        const char* const whole = tr(rest);
        if (whole != rest) {
            out += whole;
            return out;
        }
        const Row* const label = longestLabelPrefix(rest);
        if (label == nullptr) break;
        out += label->ja;
        rest += std::strlen(label->en);
    }
    if (*rest == '\0') return out;
    if (out.empty()) return text;
    out += rest;
    return out;
}

std::size_t translationCount() {
    return g_language == Language::Japanese ? kJapaneseRowCount : 0u;
}

}
