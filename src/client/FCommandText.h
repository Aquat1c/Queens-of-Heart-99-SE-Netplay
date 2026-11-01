#pragma once

#include <string>
#include <string_view>

namespace qclient::fcommand {

inline void appendCrLfLine(std::string& output, std::string_view line) {
    while (!line.empty()
           && (line.back() == '\r' || line.back() == '\n')) {
        line.remove_suffix(1u);
    }
    output.append(line.data(), line.size());
    output.append("\r\n");
}

// The native tokenizer separates lines only on CRLF; a bare LF is not a separator.
inline std::string canonicalCrLfText(std::string_view input) {
    std::string output;
    output.reserve(input.size() + 2u);
    std::size_t begin = 0u;
    while (begin < input.size()) {
        std::size_t end = begin;
        while (end < input.size()
               && input[end] != '\r' && input[end] != '\n') {
            ++end;
        }
        appendCrLfLine(output, input.substr(begin, end - begin));
        if (end == input.size()) break;
        if (input[end] == '\r' && end + 1u < input.size()
            && input[end + 1u] == '\n') {
            ++end;
        }
        begin = end + 1u;
    }
    return output;
}

inline bool hasCanonicalCrLfLines(std::string_view input) {
    return canonicalCrLfText(input) == input;
}

}
