#pragma once

#include "input/InputWorkers.h"

namespace qinput {

enum class SdlBootstrapResult {
    NotRequired = 0,
    Ready = 1,
    UnavailableBackend = 2,
    ForeignOwner = 3,
};

SdlBootstrapResult sdlBootstrapOnMainThread();

void sdlTeardownOnMainThread();

void sdlPreloadRuntime();

void sdlWarmupAsync();
void sdlWaitForWarmup();
void sdlReleaseForLaunch();

IControllerBackend* createControllerBackend(void* focusWindow);
const char* controllerBackendName();

bool standardizedBackendAvailable();

void sdlVersionText(char* out, unsigned int capacity);

}
