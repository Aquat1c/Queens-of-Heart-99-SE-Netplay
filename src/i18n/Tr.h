#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace qi18n {

enum class Language : uint8_t {
    English = 0,
    Japanese = 1,
};

bool setLanguage(Language language);
bool setLanguageFromIniValue(int value);
Language language();

const char* languageName(Language language);

const char* tr(const char* english);

std::string trText(const std::string& text);

std::size_t translationCount();

}
