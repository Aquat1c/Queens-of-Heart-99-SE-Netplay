#pragma once

#include <cstdint>
#include <vector>

#include "client/spectator/ResilientTopologyAuthority.h"
#include "net/Protocol.h"
#include "spectator/SpectatorGrant.h"

namespace qclient::spectator::topology::controlv2 {

void wipeControlBuffer(std::vector<uint8_t>& bytes);

bool endpointBindingToWire(const EndpointBinding& binding,
                           qnet::SpectateTopologyEndpointBindingWire& out);

bool infoFromEnrollment(uint64_t requestNonce,
                        const TopologyEnrollment& enrollment,
                        qnet::SpectateTopologyInfoV2& out);

bool signInfo(qnet::SpectateTopologyInfoV2& info,
              const qnetplay::spectator::SpectatorGrant& grant);

bool verifyProofTag(const qnet::SpectateTopologyProofV2& proof,
                    const qnetplay::spectator::SpectatorGrant& grant);

bool validateProof(const qnet::SpectateTopologyProofV2& proof,
                   const qnet::ClientIntent& intent,
                   const qnet::SpectateInfo& answer,
                   const qnet::SpectateTopologyChallengeV2& challenge,
                   qnetplay::spectator::SpectatorGrant& grant);

}
