#include "TestHarness.h"

#include "audio/WaveMetadata.h"

#include <cstring>
#include <vector>

using namespace qaudio;

namespace {

void putLe32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 24));
}
void putLe16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}
void putTag(std::vector<uint8_t>& out, const char* tag) {
    out.insert(out.end(), tag, tag + 4);
}

std::vector<uint8_t> buildWave(uint32_t byteRate, uint16_t blockAlign,
                               uint16_t bits, uint32_t dataBytes,
                               bool padFinalOddChunk,
                               bool extraChunkBeforeData = false) {
    std::vector<uint8_t> wave;
    putTag(wave, "RIFF");
    putLe32(wave, 0);
    putTag(wave, "WAVE");
    putTag(wave, "fmt ");
    putLe32(wave, 16);
    putLe16(wave, 1);
    putLe16(wave, bits == 8 ? 1 : 2);
    putLe32(wave, byteRate / (blockAlign ? blockAlign : 1));
    putLe32(wave, byteRate);
    putLe16(wave, blockAlign);
    putLe16(wave, bits);
    if (extraChunkBeforeData) {
        putTag(wave, "junk");
        putLe32(wave, 3);
        wave.push_back(1);
        wave.push_back(2);
        wave.push_back(3);
        wave.push_back(0);
    }
    putTag(wave, "data");
    putLe32(wave, dataBytes);
    for (uint32_t i = 0; i < dataBytes; ++i)
        wave.push_back(static_cast<uint8_t>(i));
    if (padFinalOddChunk && (dataBytes & 1u) != 0u)
        wave.push_back(0);
    const uint32_t riffSize = static_cast<uint32_t>(wave.size()) - 8u;
    wave[4] = static_cast<uint8_t>(riffSize);
    wave[5] = static_cast<uint8_t>(riffSize >> 8);
    wave[6] = static_cast<uint8_t>(riffSize >> 16);
    wave[7] = static_cast<uint8_t>(riffSize >> 24);
    return wave;
}

}

QNP_TEST(WaveFactsParseAndCanonicalDuration) {
    const auto wave = buildWave(44100, 2, 16, 44100, true);
    const WaveFacts facts = parseWaveFacts(wave.data(), wave.size());
    QNP_REQUIRE(facts.valid);
    QNP_CHECK(facts.byteRate == 44100u);
    QNP_CHECK(facts.blockAlign == 2u);
    QNP_CHECK(facts.bitsPerSample == 16u);
    QNP_CHECK(facts.dataBytes == 44100u);
    QNP_CHECK(facts.durationFrames == 61u);
}

QNP_TEST(WaveFactsAcceptUnpaddedFinalOddDataChunk) {
    const auto padded = buildWave(22050, 1, 8, 1001, true);
    const auto unpadded = buildWave(22050, 1, 8, 1001, false);
    QNP_CHECK(parseWaveFacts(padded.data(), padded.size()).valid);
    const WaveFacts facts = parseWaveFacts(unpadded.data(), unpadded.size());
    QNP_REQUIRE(facts.valid);
    QNP_CHECK(facts.dataBytes == 1001u);
    QNP_CHECK(facts.bitsPerSample == 8u);
}

QNP_TEST(WaveFactsSkipPaddedInteriorOddChunk) {
    const auto wave = buildWave(22050, 2, 16, 512, true, true);
    const WaveFacts facts = parseWaveFacts(wave.data(), wave.size());
    QNP_REQUIRE(facts.valid);
    QNP_CHECK(facts.dataBytes == 512u);
}

QNP_TEST(WaveFactsRejectMalformedImages) {
    auto wave = buildWave(22050, 2, 16, 64, true);
    QNP_CHECK(!parseWaveFacts(nullptr, 0).valid);
    QNP_CHECK(!parseWaveFacts(wave.data(), 8).valid);
    std::vector<uint8_t> badMagic = wave;
    badMagic[0] = 'X';
    QNP_CHECK(!parseWaveFacts(badMagic.data(), badMagic.size()).valid);
    std::vector<uint8_t> truncated = wave;
    truncated.resize(truncated.size() - 8);
    QNP_CHECK(!parseWaveFacts(truncated.data(), truncated.size()).valid);
}

QNP_TEST(WaveSilenceByteDependsOnPcmDepth) {
    QNP_CHECK(silenceByteFor(8) == 0x80);
    QNP_CHECK(silenceByteFor(16) == 0x00);
}

QNP_TEST(WaveResumeCursorAlignmentAndExpiry) {
    WaveFacts facts{};
    facts.valid = true;
    facts.dataBytes = 44100;
    facts.byteRate = 44100;
    facts.blockAlign = 4;
    facts.durationFrames = 61;

    const uint32_t half = resumeCursorBytes(facts, 30);
    QNP_REQUIRE(half != kCursorExpired);
    QNP_CHECK(half % 4u == 0u);
    QNP_CHECK(half > 21000u && half < 23000u);

    QNP_CHECK(resumeCursorBytes(facts, 61) == kCursorExpired);
    QNP_CHECK(resumeCursorBytes(facts, 200) == kCursorExpired);

    facts.looping = true;
    const uint32_t wrapped = resumeCursorBytes(facts, 100);
    QNP_REQUIRE(wrapped != kCursorExpired);
    QNP_CHECK(wrapped < facts.dataBytes);

    WaveFacts invalid{};
    QNP_CHECK(resumeCursorBytes(invalid, 1) == kCursorExpired);
}
