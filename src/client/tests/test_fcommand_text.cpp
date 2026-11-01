#include "../../netplay/tests/TestHarness.h"

#include "../FCommandText.h"

#include <string>

QNP_TEST(FCommandTextCanonicalizesEverySeparatorToCrLf) {
    const std::string mixed =
        "ExtraFunctionEnable TRUE\n"
        "SystemPause FALSE\r\n"
        "NoSound FALSE\r"
        "BgmPlayType Buffer";
    const std::string expected =
        "ExtraFunctionEnable TRUE\r\n"
        "SystemPause FALSE\r\n"
        "NoSound FALSE\r\n"
        "BgmPlayType Buffer\r\n";

    QNP_CHECK(qclient::fcommand::canonicalCrLfText(mixed) == expected);
    QNP_CHECK(qclient::fcommand::hasCanonicalCrLfLines(expected));
    QNP_CHECK(!qclient::fcommand::hasCanonicalCrLfLines(mixed));
}

QNP_TEST(FCommandTextAppendRemovesRetainedGetlineCarriageReturn) {
    std::string output;
    qclient::fcommand::appendCrLfLine(
        output, "ExtraFunctionEnable TRUE\r");
    qclient::fcommand::appendCrLfLine(output, "SystemPause FALSE");

    QNP_CHECK(output
              == "ExtraFunctionEnable TRUE\r\nSystemPause FALSE\r\n");
    QNP_CHECK(output.find("\r\r\n") == std::string::npos);
}
