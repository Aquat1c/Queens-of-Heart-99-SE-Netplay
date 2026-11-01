#include "../TopologyAuthoritySignature.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

namespace topology = qclient::spectator::topology;

int g_failures = 0;

#define CHECK_TRUE(expression)                                               \
    do {                                                                     \
        if (!(expression)) {                                                 \
            std::fprintf(stderr, "%s:%d CHECK failed: %s\n",              \
                         __FILE__, __LINE__, #expression);                  \
            ++g_failures;                                                    \
        }                                                                    \
    } while (false)

bool allZero(const std::uint8_t* bytes, const std::size_t count) {
    if (bytes == nullptr) return false;
    for (std::size_t index = 0u; index < count; ++index) {
        if (bytes[index] != 0u) return false;
    }
    return true;
}

void testRoundTripAndTamperRejection() {
    topology::TopologyAuthoritySigner signer;
    CHECK_TRUE(!signer.ready());
    CHECK_TRUE(signer.generate());
    CHECK_TRUE(signer.ready());

    topology::TopologyAuthorityPublicBlob publicBlob{};
    CHECK_TRUE(signer.publicBlob(publicBlob));

    topology::TopologyAuthoritySignatureVerifier verifier;
    CHECK_TRUE(verifier.initialize(publicBlob));
    CHECK_TRUE(verifier.ready());

    const std::array<std::uint8_t, 19u> transcript{{
        0x54u, 0x4Fu, 0x50u, 0x4Fu, 0x4Cu, 0x4Fu, 0x47u, 0x59u,
        0x2Du, 0x41u, 0x54u, 0x54u, 0x45u, 0x53u, 0x54u, 0x2Du,
        0x56u, 0x31u, 0x00u,
    }};
    topology::TopologyAuthoritySignature signature{};
    CHECK_TRUE(signer.sign(transcript.data(), transcript.size(), signature));
    CHECK_TRUE(!allZero(signature.data(), signature.size()));
    CHECK_TRUE(verifier.verify(transcript.data(), transcript.size(), signature));

    auto tamperedTranscript = transcript;
    tamperedTranscript[3u] ^= 0x80u;
    CHECK_TRUE(!verifier.verify(
        tamperedTranscript.data(), tamperedTranscript.size(), signature));

    auto tamperedSignature = signature;
    tamperedSignature[17u] ^= 0x01u;
    CHECK_TRUE(!verifier.verify(
        transcript.data(), transcript.size(), tamperedSignature));

    topology::TopologyAuthoritySigner otherSigner;
    CHECK_TRUE(otherSigner.generate());
    topology::TopologyAuthorityPublicBlob otherPublicBlob{};
    CHECK_TRUE(otherSigner.publicBlob(otherPublicBlob));
    topology::TopologyAuthoritySignatureVerifier otherVerifier;
    CHECK_TRUE(otherVerifier.initialize(otherPublicBlob));
    CHECK_TRUE(!otherVerifier.verify(
        transcript.data(), transcript.size(), signature));
}

void testMalformedInputsFailClosed() {
    topology::TopologyAuthoritySigner signer;
    topology::TopologyAuthorityPublicBlob publicBlob{};
    topology::TopologyAuthoritySignature signature{};
    const std::array<std::uint8_t, 1u> input{{0xA5u}};

    CHECK_TRUE(!signer.publicBlob(publicBlob));
    CHECK_TRUE(!signer.sign(input.data(), input.size(), signature));
    CHECK_TRUE(allZero(signature.data(), signature.size()));
    CHECK_TRUE(signer.generate());
    CHECK_TRUE(signer.publicBlob(publicBlob));
    CHECK_TRUE(signer.sign(input.data(), input.size(), signature));

    topology::TopologyAuthoritySignatureVerifier verifier;
    CHECK_TRUE(!verifier.initialize(nullptr, publicBlob.size()));
    CHECK_TRUE(!verifier.ready());
    CHECK_TRUE(!verifier.initialize(publicBlob.data(), publicBlob.size() - 1u));
    CHECK_TRUE(!verifier.ready());

    auto malformedMagic = publicBlob;
    malformedMagic[0u] ^= 0x01u;
    CHECK_TRUE(!verifier.initialize(malformedMagic));
    CHECK_TRUE(!verifier.ready());

    topology::TopologyAuthorityPublicBlob zeroBlob{};
    CHECK_TRUE(!verifier.initialize(zeroBlob));
    CHECK_TRUE(!verifier.ready());
    CHECK_TRUE(verifier.initialize(publicBlob));

    CHECK_TRUE(!verifier.verify(nullptr, input.size(), signature));
    CHECK_TRUE(!verifier.verify(input.data(), 0u, signature));
    CHECK_TRUE(!verifier.verify(input.data(), input.size(), nullptr,
                                signature.size()));
    CHECK_TRUE(!verifier.verify(input.data(), input.size(), signature.data(),
                                signature.size() - 1u));

    std::vector<std::uint8_t> oversized(
        topology::kTopologyAuthoritySignatureMaxInputBytes + 1u, 0x5Au);
    CHECK_TRUE(!signer.sign(oversized.data(), oversized.size(), signature));
    CHECK_TRUE(allZero(signature.data(), signature.size()));
    CHECK_TRUE(!verifier.verify(oversized.data(), oversized.size(),
                                signature.data(), signature.size()));
}

void testReplacementIsExplicitAndOldPublicKeyCannotVerify() {
    topology::TopologyAuthoritySigner signer;
    CHECK_TRUE(signer.generate());
    const std::array<std::uint8_t, 4u> input{{1u, 2u, 3u, 4u}};
    topology::TopologyAuthorityPublicBlob firstPublic{};
    topology::TopologyAuthoritySignature firstSignature{};
    CHECK_TRUE(signer.publicBlob(firstPublic));
    CHECK_TRUE(signer.sign(input.data(), input.size(), firstSignature));

    topology::TopologyAuthoritySignatureVerifier firstVerifier;
    CHECK_TRUE(firstVerifier.initialize(firstPublic));
    CHECK_TRUE(firstVerifier.verify(input.data(), input.size(), firstSignature));

    CHECK_TRUE(signer.generate());
    topology::TopologyAuthorityPublicBlob replacementPublic{};
    topology::TopologyAuthoritySignature replacementSignature{};
    CHECK_TRUE(signer.publicBlob(replacementPublic));
    CHECK_TRUE(signer.sign(input.data(), input.size(), replacementSignature));
    CHECK_TRUE(!firstVerifier.verify(
        input.data(), input.size(), replacementSignature));
}

}

int main() {
    testRoundTripAndTamperRejection();
    testMalformedInputsFailClosed();
    testReplacementIsExplicitAndOldPublicKeyCannotVerify();
    if (g_failures != 0) {
        std::fprintf(stderr,
                     "topology authority signature tests FAILED: %d\n",
                     g_failures);
        return 1;
    }
    std::puts("topology authority signature tests passed");
    return 0;
}
