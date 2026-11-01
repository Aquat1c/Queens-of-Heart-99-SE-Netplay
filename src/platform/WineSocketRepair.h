#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>

#include <string>

namespace qplat {

bool protocolInfoNeedsUdpRepair(const WSAPROTOCOL_INFOW& info);

bool repairDuplicatedUdpProtocolInfo(WSAPROTOCOL_INFOW& info);

std::string describeProtocolInfo(const WSAPROTOCOL_INFOW& info);

}
