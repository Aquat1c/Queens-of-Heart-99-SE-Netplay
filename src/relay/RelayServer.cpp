#include "RelayServiceVerb.h"

#include <cstdio>

int main(const int argc, char** const argv) {
    std::fprintf(
        stderr,
        "qoh99relay.exe is deprecated and will be removed after this "
        "release.\nUse: qoh99caster relay [--bind IPV4] [--port N]\n");
    return qrelay::runRelayVerb(argc, argv);
}
