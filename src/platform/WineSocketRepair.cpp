#include "WineSocketRepair.h"

#include <cstdio>

namespace qplat {

bool protocolInfoNeedsUdpRepair(const WSAPROTOCOL_INFOW& info) {
    return info.iAddressFamily == 0
        || info.iSocketType == 0
        || info.iProtocol == 0;
}

bool repairDuplicatedUdpProtocolInfo(WSAPROTOCOL_INFOW& info) {
    bool changed = false;
    if (info.iAddressFamily == 0) {
        info.iAddressFamily = AF_INET;
        changed = true;
    }
    if (info.iSocketType == 0) {
        info.iSocketType = SOCK_DGRAM;
        changed = true;
    }
    if (info.iProtocol == 0) {
        info.iProtocol = IPPROTO_UDP;
        changed = true;
    }
    return changed;
}

std::string describeProtocolInfo(const WSAPROTOCOL_INFOW& info) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "af=%d type=%d proto=%d",
                  info.iAddressFamily, info.iSocketType, info.iProtocol);
    return buf;
}

}
