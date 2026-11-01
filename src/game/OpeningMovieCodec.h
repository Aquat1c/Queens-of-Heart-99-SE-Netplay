#pragma once

namespace qmovie {

bool registerOpeningMovieCodec();

bool openingMovieCodecRegistered();

struct OpeningMovieCodecStats {
    unsigned instancesOpened = 0u;
    unsigned framesDecoded = 0u;
    unsigned framesRejected = 0u;
    unsigned framesOutOfSequence = 0u;
};
OpeningMovieCodecStats openingMovieCodecStats();

constexpr const char* kOpeningMovieLosslessName = "OPENING_LOSSLESS.AVI";

}
