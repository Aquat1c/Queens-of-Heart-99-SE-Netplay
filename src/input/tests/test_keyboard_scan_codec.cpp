#include "TestHarness.h"

#include "input/KeyboardScanCodec.h"

#include <cstring>

using namespace qinput;

QNP_TEST(ScanCodecBasicMapping) {
    DikResolution z = dikFromRawScan({0x2C, false, false});
    QNP_REQUIRE(z.valid);
    QNP_CHECK(z.dik == 0x2C);
    QNP_CHECK(std::strcmp(dikDisplayLabel(z.dik), "Z") == 0);
    DikResolution up = dikFromRawScan({0x48, true, false});
    QNP_REQUIRE(up.valid);
    QNP_CHECK(up.dik == 0xC8);
    QNP_CHECK(dikIsExtended(up.dik));
}

QNP_TEST(ScanCodecModifiersDistinct) {
    const DikResolution leftShift = dikFromRawScan({0x2A, false, false});
    const DikResolution rightShift = dikFromRawScan({0x36, false, false});
    const DikResolution leftCtrl = dikFromRawScan({0x1D, false, false});
    const DikResolution rightCtrl = dikFromRawScan({0x1D, true, false});
    const DikResolution leftAlt = dikFromRawScan({0x38, false, false});
    const DikResolution rightAlt = dikFromRawScan({0x38, true, false});
    QNP_REQUIRE(leftShift.valid && rightShift.valid);
    QNP_REQUIRE(leftCtrl.valid && rightCtrl.valid);
    QNP_REQUIRE(leftAlt.valid && rightAlt.valid);
    QNP_CHECK(leftShift.dik != rightShift.dik);
    QNP_CHECK(leftCtrl.dik != rightCtrl.dik);
    QNP_CHECK(leftAlt.dik != rightAlt.dik);
}

QNP_TEST(ScanCodecNumpadVersusNavigation) {
    const DikResolution numpad8 = dikFromRawScan({0x48, false, false});
    const DikResolution upArrow = dikFromRawScan({0x48, true, false});
    QNP_REQUIRE(numpad8.valid && upArrow.valid);
    QNP_CHECK(numpad8.dik == 0x48);
    QNP_CHECK(upArrow.dik == 0xC8);
    QNP_CHECK(std::strcmp(dikDisplayLabel(numpad8.dik), "Numpad8") == 0);
    QNP_CHECK(std::strcmp(dikDisplayLabel(upArrow.dik), "Up") == 0);
    const DikResolution numpadEnter = dikFromRawScan({0x1C, true, false});
    QNP_REQUIRE(numpadEnter.valid);
    QNP_CHECK(numpadEnter.dik == 0x9C);
    const DikResolution del = dikFromRawScan({0x53, true, false});
    QNP_REQUIRE(del.valid);
    QNP_CHECK(del.dik == 0xD3);
    const DikResolution numpadDot = dikFromRawScan({0x53, false, false});
    QNP_REQUIRE(numpadDot.valid);
    QNP_CHECK(numpadDot.dik == 0x53);
}

QNP_TEST(ScanCodecMultiPacketSpecials) {
    const DikResolution pause = dikFromRawScan({0x1D, false, true});
    QNP_REQUIRE(pause.valid);
    QNP_CHECK(pause.dik == 0xC5);
    QNP_CHECK(std::strcmp(dikDisplayLabel(pause.dik), "Pause") == 0);
    QNP_CHECK(!dikFromRawScan({0x45, false, true}).valid);
    QNP_CHECK(!dikFromRawScan({0x2A, true, false}).valid);
    const DikResolution printScreen = dikFromRawScan({0x37, true, false});
    QNP_REQUIRE(printScreen.valid);
    QNP_CHECK(printScreen.dik == 0xB7);
}

QNP_TEST(ScanCodecLabelsRoundTrip) {
    for (uint16_t make = 1; make <= 0x58; ++make) {
        const DikResolution plain = dikFromRawScan({make, false, false});
        if (!plain.valid) continue;
        QNP_CHECK(std::strcmp(dikDisplayLabel(plain.dik), "?") != 0);
    }
    QNP_CHECK(!dikFromRawScan({0, false, false}).valid);
    QNP_CHECK(!dikFromRawScan({0x1FF, false, false}).valid);
}
