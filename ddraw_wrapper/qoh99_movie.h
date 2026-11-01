#ifndef QOH99_MOVIE_H
#define QOH99_MOVIE_H

#include "qoh99_movie_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

void qoh99_movie_install(void);

void qoh99_movie_shutdown(void);

Qoh99MovieDecision qoh99_movie_decision(void);

void qoh99_movie_set_game_resolution(int width, int height);

void qoh99_movie_window_resized(void);

int qoh99_movie_is_playing(void);

#ifdef __cplusplus
}
#endif

#endif
