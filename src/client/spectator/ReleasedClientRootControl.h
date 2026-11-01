#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "client/spectator/ResilientTopologyAuthority.h"
#include "net/Protocol.h"

namespace qclient::spectator::topology::clientroot {

void wipeControlBuffer(std::vector<uint8_t>& bytes);

enum class Direction : uint8_t {
    Claim = 1,
    Enrollment = 2,
};

bool makeControlTag(const uint8_t* token, size_t tokenBytes,
                    Direction direction, const uint8_t* packedBodyWithoutTag,
                    size_t packedBodyWithoutTagBytes,
                    std::array<uint8_t, 16u>& out);

bool verifyControlTag(const uint8_t* token, size_t tokenBytes,
                      Direction direction,
                      const uint8_t* packedBodyWithoutTag,
                      size_t packedBodyWithoutTagBytes,
                      const std::array<uint8_t, 16u>& expected);

bool claimFromWire(const qnet::ClientRootClaim& wire,
                   uint8_t expectedRemotePlayerRole, uint64_t sessionId,
                   uint64_t sessionEpoch, ClientPlayerRootClaim& out);

bool enrollmentToWire(uint64_t requestNonce,
                      const ClientPlayerRootEnrollment& enrollment,
                      const uint8_t* grantBlob, size_t grantBlobBytes,
                      qnet::ClientRootEnrollment& out);

bool enrollmentFromWire(const qnet::ClientRootEnrollment& wire,
                        uint64_t expectedRequestNonce,
                        std::array<uint8_t,
                                   qnet::kClientRootCredentialBytes>& credential,
                        std::vector<uint8_t>& grantBlob);

}
