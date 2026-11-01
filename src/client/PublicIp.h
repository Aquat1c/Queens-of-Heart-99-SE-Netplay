/* ADVISORY ONLY: nothing that decides a route may read this value; use qnet::relay::ObservedRegistration (net/RelayClient.h). */
#pragma once

#include <cstdint>
#include <string>

namespace qclient::public_ip {

void start();
void refresh();
bool pending();
std::string value();

bool advisoryOnly();

bool httpBackendAvailable();

const char* httpBackendStatus();

bool normalizeIpv4(const std::string& text, std::string& canonical);
bool normalizeHttpIpv4Response(std::uint32_t statusCode, bool cleanEof,
                               const std::string& body,
                               std::string& canonical);

}
