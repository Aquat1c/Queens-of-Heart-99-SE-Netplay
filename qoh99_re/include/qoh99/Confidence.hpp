#pragma once

namespace qoh99 {

enum class Confidence {
    RuntimeVerified,
    StaticAnalysis,
    StrongLikely,
    Hypothesis,
    Unknown
};

inline const char* toString(Confidence c) {
    switch (c) {
        case Confidence::RuntimeVerified: return "RuntimeVerified";
        case Confidence::StaticAnalysis: return "StaticAnalysis";
        case Confidence::StrongLikely:    return "StrongLikely";
        case Confidence::Hypothesis:      return "Hypothesis";
        default:                          return "Unknown";
    }
}

}
