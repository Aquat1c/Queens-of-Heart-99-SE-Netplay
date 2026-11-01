#include "TestHarness.h"

#include "client/RawInputCapture.h"
#include "client/SdlUi.h"

#include <string>
#include <vector>

using namespace qclient::ui;
using qclient::CapturedKey;
using qclient::RawKeyboardPacket;
using qclient::ScanNormalizer;
using qclient::kRawKeyBreak;
using qclient::kRawKeyE0;
using qclient::kRawKeyE1;
using qclient::kRawKeyMake;

namespace {

const char* const kHi = "\xE3\x81\x82";
const char* const kNihon = "\xE6\x97\xA5\xE6\x9C\xAC";
const char* const kEacute = "\xC3\xA9";
const char* const kEmoji = "\xF0\x9F\x8E\xAE";

MainMenuOptions fullyEnabledMainMenu() {
    MainMenuOptions options;
    options.nickname = "Tester";
    options.hostPort = 7500;
    options.developerMode = true;
    options.gameConfigured = true;
    options.sessionRunning = false;
    return options;
}

bool focusByKeyboard(NavigationModel& model, ActionId wanted) {
    const std::size_t options = model.selectableCount();
    if (options == 0) return false;
    for (std::size_t step = 0; step <= options; ++step) {
        if (model.focusedAction() == wanted) return true;
        if (!model.moveFocus(1)) return false;
    }
    return false;
}

}

QNP_TEST(SdlUiEveryPageReachableByKeyboardOnly) {
    const Page targets[] = {
        Page::Host, Page::Join, Page::Spectate, Page::Replays,
        Page::Offline, Page::Controls, Page::Settings, Page::Diagnostics,
    };
    QNP_CHECK(sizeof(targets) / sizeof(targets[0])
              == static_cast<std::size_t>(Page::Count) - 1);

    for (Page target : targets) {
        NavigationModel model;
        model.reset(Page::Main);
        model.setView(buildMainMenu(fullyEnabledMainMenu()));
        QNP_REQUIRE(model.page() == Page::Main);

        bool reached = false;
        const std::size_t options = model.selectableCount();
        QNP_REQUIRE(options > 0);
        for (std::size_t step = 0; step <= options; ++step) {
            const ActivateResult result = model.activate();
            if (result.accepted && pageForAction(result.action) == target) {
                QNP_CHECK(model.navigate(pageForAction(result.action)));
                reached = model.page() == target;
                break;
            }
            if (!model.moveFocus(1)) break;
        }
        QNP_CHECK(reached);
    }
}

QNP_TEST(SdlUiOfflineSubPageIsKeyboardReachable) {
    NavigationModel model;
    model.reset(Page::Main);
    model.setView(buildMainMenu(fullyEnabledMainMenu()));
    QNP_REQUIRE(focusByKeyboard(model, ActionId::GoOffline));
    QNP_REQUIRE(model.activate().accepted);
    QNP_REQUIRE(model.navigate(Page::Offline));

    OfflineMenuOptions offline;
    offline.gameConfigured = true;
    model.setView(buildOfflineMenu(offline));

    const ActionId modes[] = { ActionId::OfflineTraining,
                               ActionId::OfflineVsPlayer,
                               ActionId::OfflineVsCpu,
                               ActionId::OfflineFreePlay };
    for (ActionId mode : modes) {
        QNP_CHECK(focusByKeyboard(model, mode));
        QNP_CHECK(model.activate().accepted);
    }
}

QNP_TEST(SdlUiFocusWrapsAndSkipsNonOptions) {
    PageView view;
    view.page = Page::Settings;
    view.items.push_back(makeHeader("Section"));
    view.items.push_back(makeAction(ActionId::SettingsEditText, "First"));
    view.items.push_back(makeSpacer());
    view.items.push_back(makeAction(ActionId::SettingsEditNumber, "Second"));
    view.items.push_back(makeHeader("Another section"));
    view.items.push_back(makeAction(ActionId::SettingsToggle, "Third"));

    NavigationModel model;
    model.setView(view);
    QNP_REQUIRE(model.focusedItem() != nullptr);
    QNP_CHECK(model.focusedAction() == ActionId::SettingsEditText);
    QNP_CHECK(model.selectableCount() == 3);

    QNP_CHECK(model.moveFocus(1));
    QNP_CHECK(model.focusedAction() == ActionId::SettingsEditNumber);
    QNP_CHECK(model.moveFocus(1));
    QNP_CHECK(model.focusedAction() == ActionId::SettingsToggle);
    QNP_CHECK(model.moveFocus(1));
    QNP_CHECK(model.focusedAction() == ActionId::SettingsEditText);
    QNP_CHECK(model.moveFocus(-1));
    QNP_CHECK(model.focusedAction() == ActionId::SettingsToggle);

    QNP_CHECK(model.moveFocus(5));
    QNP_CHECK(model.focusedItem() != nullptr);
    QNP_CHECK(model.focusedItem()->selectable());

    PageView inert;
    inert.page = Page::Settings;
    inert.items.push_back(makeHeader("Only a title"));
    inert.items.push_back(makeSpacer());
    NavigationModel empty;
    empty.setView(inert);
    QNP_CHECK(empty.selectableCount() == 0);
    QNP_CHECK(empty.focusedItem() == nullptr);
    QNP_CHECK(!empty.moveFocus(1));
    QNP_CHECK(!empty.activate().accepted);
    QNP_CHECK(empty.activate().reason == DisableReason::NoSelection);
}

QNP_TEST(SdlUiBackSemantics) {
    NavigationModel model;
    model.reset(Page::Main);
    QNP_CHECK(model.depth() == 1);
    QNP_CHECK(!model.back());

    QNP_CHECK(model.navigate(Page::Settings));
    QNP_CHECK(model.page() == Page::Settings);
    QNP_CHECK(model.depth() == 2);
    QNP_CHECK(!model.navigate(Page::Settings));
    QNP_CHECK(model.navigate(Page::Controls));
    QNP_CHECK(model.depth() == 3);

    QNP_CHECK(model.back());
    QNP_CHECK(model.page() == Page::Settings);
    QNP_CHECK(model.back());
    QNP_CHECK(model.page() == Page::Main);
    QNP_CHECK(!model.back());

    QNP_CHECK(model.navigate(Page::Replays));
    model.reset(Page::Main);
    QNP_CHECK(model.depth() == 1);
    QNP_CHECK(model.page() == Page::Main);
}

QNP_TEST(SdlUiDisabledActionsExposeTypedReason) {
    MainMenuOptions options = fullyEnabledMainMenu();
    options.gameConfigured = false;
    options.gameProblem = "GAME.EXE was not found at D:\\QOH99";
    options.developerMode = false;

    NavigationModel model;
    model.reset(Page::Main);
    model.setView(buildMainMenu(options));

    QNP_REQUIRE(focusByKeyboard(model, ActionId::GoHost));
    const ActivateResult host = model.activate();
    QNP_CHECK(!host.accepted);
    QNP_CHECK(host.action == ActionId::GoHost);
    QNP_CHECK(host.reason == DisableReason::GameNotConfigured);
    QNP_CHECK(!host.reasonText().empty());
    QNP_CHECK(host.reasonText().find("GAME.EXE") != std::string::npos);

    QNP_REQUIRE(focusByKeyboard(model, ActionId::GoDiagnostics));
    const ActivateResult diagnostics = model.activate();
    QNP_CHECK(!diagnostics.accepted);
    QNP_CHECK(diagnostics.reason == DisableReason::DeveloperModeRequired);
    QNP_CHECK(!diagnostics.reasonText().empty());

    QNP_REQUIRE(focusByKeyboard(model, ActionId::GoReplays));
    QNP_CHECK(model.activate().accepted);

    PageView view;
    view.page = Page::Replays;
    Item item = makeAction(ActionId::ReplayWatch, "Watch");
    item.enabled = false;
    view.items.push_back(item);
    NavigationModel other;
    other.setView(view);
    const ActivateResult generic = other.activate();
    QNP_CHECK(!generic.accepted);
    QNP_CHECK(generic.reason == DisableReason::Unavailable);
    QNP_CHECK(!generic.reasonText().empty());

    for (std::size_t i = 0; i < static_cast<std::size_t>(DisableReason::Count);
         ++i) {
        const char* text = describe(static_cast<DisableReason>(i));
        QNP_CHECK(text != nullptr && text[0] != '\0');
    }
}

QNP_TEST(SdlUiFocusSurvivesLiveRebuild) {
    PageView first;
    first.page = Page::Replays;
    first.items.push_back(makeAction(ActionId::ReplaySelect, "Match A"));
    first.items.back().index = 10;
    first.items.push_back(makeAction(ActionId::ReplaySelect, "Match B"));
    first.items.back().index = 11;

    NavigationModel model;
    model.setView(first);
    QNP_REQUIRE(model.moveFocus(1));
    QNP_CHECK(model.focusedIndex() == 11);

    PageView second;
    second.page = Page::Replays;
    second.items.push_back(makeAction(ActionId::ReplaySelect, "Match NEW"));
    second.items.back().index = 12;
    second.items.push_back(makeAction(ActionId::ReplaySelect, "Match A"));
    second.items.back().index = 10;
    second.items.push_back(makeAction(ActionId::ReplaySelect, "Match B"));
    second.items.back().index = 11;
    model.setView(second);
    QNP_CHECK(model.focusedIndex() == 11);
    QNP_CHECK(model.focus() == 2);

    PageView settings;
    settings.page = Page::Settings;
    settings.items.push_back(makeAction(ActionId::SettingsEditText, "Nickname"));
    settings.items.push_back(makeAction(ActionId::SettingsEditNumber, "Port"));
    model.setView(settings);
    QNP_REQUIRE(model.moveFocus(1));
    QNP_CHECK(model.focus() == 1);
    model.setView(second);
    QNP_CHECK(model.focusedIndex() == 11);
    model.setView(settings);
    QNP_CHECK(model.focus() == 1);
}

QNP_TEST(SdlUiTextEntryHandlesMultiByteInsertAndDelete) {
    TextEntry entry(64);
    QNP_CHECK(entry.insert("ab"));
    QNP_CHECK(entry.insert(kHi));
    QNP_CHECK(entry.insert(kEacute));
    QNP_CHECK(entry.insert(kEmoji));
    QNP_CHECK(entry.text() == std::string("ab") + kHi + kEacute + kEmoji);
    QNP_CHECK(entry.lengthCodePoints() == 5);
    QNP_CHECK(entry.caretBytes() == entry.text().size());

    QNP_CHECK(entry.backspace());
    QNP_CHECK(entry.text() == std::string("ab") + kHi + kEacute);
    QNP_CHECK(entry.lengthCodePoints() == 4);
    QNP_CHECK(utf8Valid(entry.text()));

    QNP_CHECK(entry.backspace());
    QNP_CHECK(entry.text() == std::string("ab") + kHi);
    QNP_CHECK(entry.backspace());
    QNP_CHECK(entry.text() == "ab");
    QNP_CHECK(entry.backspace());
    QNP_CHECK(entry.backspace());
    QNP_CHECK(entry.text().empty());
    QNP_CHECK(!entry.backspace());
}

QNP_TEST(SdlUiTextEntryCaretMovesOverCodePoints) {
    TextEntry entry(64);
    entry.setText(std::string("a") + kNihon + "b");
    QNP_CHECK(entry.lengthCodePoints() == 4);
    QNP_CHECK(entry.caretBytes() == entry.text().size());
    QNP_CHECK(entry.caretCodePoints() == 4);

    QNP_CHECK(entry.moveLeft());
    QNP_CHECK(entry.caretCodePoints() == 3);
    QNP_CHECK(entry.moveLeft());
    QNP_CHECK(entry.caretCodePoints() == 2);
    QNP_CHECK(entry.caretBytes() == 4);
    QNP_CHECK(entry.moveLeft());
    QNP_CHECK(entry.caretBytes() == 1);
    QNP_CHECK(entry.moveLeft());
    QNP_CHECK(entry.caretBytes() == 0);
    QNP_CHECK(!entry.moveLeft());

    QNP_CHECK(entry.moveRight());
    QNP_CHECK(entry.insert("X"));
    QNP_CHECK(entry.text() == std::string("aX") + kNihon + "b");
    QNP_CHECK(utf8Valid(entry.text()));

    QNP_CHECK(entry.deleteForward());
    QNP_CHECK(entry.text() == std::string("aX") + "\xE6\x9C\xAC" + "b");
    QNP_CHECK(utf8Valid(entry.text()));

    entry.moveHome();
    QNP_CHECK(entry.caretBytes() == 0);
    entry.moveEnd();
    QNP_CHECK(entry.caretBytes() == entry.text().size());
    QNP_CHECK(!entry.moveRight());
}

QNP_TEST(SdlUiTextEntryEnforcesLengthLimit) {
    TextEntry entry(7);
    QNP_CHECK(entry.insert(kHi));
    QNP_CHECK(entry.insert(kHi));
    QNP_CHECK(entry.text().size() == 6);
    QNP_CHECK(entry.lastRejection() == TextRejection::None);

    QNP_CHECK(!entry.insert(kHi));
    QNP_CHECK(entry.text().size() == 6);
    QNP_CHECK(entry.lastRejection() == TextRejection::LengthLimit);
    QNP_CHECK(utf8Valid(entry.text()));

    QNP_CHECK(entry.insert("z"));
    QNP_CHECK(entry.text().size() == 7);
    QNP_CHECK(entry.full());
    QNP_CHECK(!entry.insert("z"));
    QNP_CHECK(entry.lastRejection() == TextRejection::LengthLimit);

    TextEntry paste(5);
    QNP_CHECK(paste.insert(std::string(kHi) + kHi + kHi));
    QNP_CHECK(paste.text() == kHi);
    QNP_CHECK(paste.lastRejection() == TextRejection::LengthLimit);
    QNP_CHECK(utf8Valid(paste.text()));
}

QNP_TEST(SdlUiTextEntryRejectsControlCharacters) {
    TextEntry entry(64);
    QNP_CHECK(!entry.insert("\x01\x02\x1F\x7F"));
    QNP_CHECK(entry.text().empty());
    QNP_CHECK(entry.lastRejection() == TextRejection::ControlCharacter);

    QNP_CHECK(entry.insert("ok\x07here"));
    QNP_CHECK(entry.text() == "okhere");
    QNP_CHECK(entry.lastRejection() == TextRejection::ControlCharacter);

    TextEntry line(64);
    QNP_CHECK(line.insert("192.168.1.10:7500\r\nrm -rf /"));
    QNP_CHECK(line.text() == "192.168.1.10:7500");

    TextEntry broken(64);
    QNP_CHECK(broken.insert(std::string("a\xE6\x97") + "b"));
    QNP_CHECK(utf8Valid(broken.text()));
    QNP_CHECK(broken.text() == "ab");
    QNP_CHECK(broken.lastRejection() == TextRejection::InvalidEncoding);

    TextEntry numeric(32);
    numeric.setNumericOnly(true);
    QNP_CHECK(numeric.insert("10.0.0.5:7500"));
    QNP_CHECK(numeric.text() == "10.0.0.5:7500");
    QNP_CHECK(!numeric.insert("abc"));
    QNP_CHECK(numeric.text() == "10.0.0.5:7500");
    QNP_CHECK(numeric.lastRejection() == TextRejection::NotPermitted);
}

QNP_TEST(SdlUiSanitizeStripsControlsAndEscapes) {
    QNP_CHECK(sanitizeText("\x1B[31mred\x1B[0m") == "red");
    QNP_CHECK(sanitizeText("\x1B]0;window title\x07" "after") == "after");
    QNP_CHECK(sanitizeText("a\x1B(Bb") == "ab");
    QNP_CHECK(sanitizeText("bell\x07 and null") == "bell and null");
    QNP_CHECK(sanitizeText("del\x7F") == "del");
    QNP_CHECK(sanitizeText("x\xC2\x9Bmy") == "xmy");
    QNP_CHECK(sanitizeText("a\tb\r") == "a b");
}

QNP_TEST(SdlUiSanitizeRoundTripsNonAscii) {
    const std::string japanese = std::string(kNihon) + kHi;
    QNP_CHECK(sanitizeText(japanese) == japanese);
    QNP_CHECK(sanitizeText(kEmoji) == std::string(kEmoji));
    QNP_CHECK(sanitizeText(kEacute) == std::string(kEacute));

    const std::string mixed = std::string("Player ") + kNihon + " (P1)";
    QNP_CHECK(sanitizeText(mixed) == mixed);
    QNP_CHECK(utf8Valid(sanitizeText(mixed)));
    QNP_CHECK(utf8CodePointCount(japanese) == 3);

    const std::string spoofed = std::string("set") + "\xE2\x80\xAE" + kNihon;
    QNP_CHECK(sanitizeText(spoofed) == std::string("set") + kNihon);
    QNP_CHECK(sanitizeText(std::string("a") + "\xEF\xBB\xBF" + "b") == "ab");
}

QNP_TEST(SdlUiSanitizeHonoursLineAndLengthPolicy) {
    TextSanitizeOptions single;
    single.singleLine = true;
    QNP_CHECK(sanitizeText("first\nsecond", single) == "first");

    TextSanitizeOptions multi;
    multi.singleLine = false;
    QNP_CHECK(sanitizeText("first\nsecond", multi) == "first\nsecond");
    QNP_CHECK(sanitizeText("first\r\nsecond", multi) == "first\nsecond");

    TextSanitizeOptions tabs;
    tabs.keepTabs = true;
    QNP_CHECK(sanitizeText("a\tb", tabs) == "a\tb");

    TextSanitizeOptions bounded;
    bounded.maxBytes = 4;
    const std::string clipped =
        sanitizeText(std::string(kHi) + kHi + kHi, bounded);
    QNP_CHECK(clipped == kHi);
    QNP_CHECK(utf8Valid(clipped));
}

namespace {

RawKeyboardPacket make(uint16_t makeCode, uint16_t flags, uint16_t vkey) {
    RawKeyboardPacket packet;
    packet.makeCode = makeCode;
    packet.flags = flags;
    packet.virtualKey = vkey;
    packet.message = (flags & kRawKeyBreak) != 0 ? 0x0101u
                                                 : 0x0100u ;
    return packet;
}

}

QNP_TEST(ScanNormalizerKeepsE0InTheKeyIdentity) {
    ScanNormalizer normalizer;
    CapturedKey key{};

    QNP_CHECK(normalizer.feed(make(0x1D, kRawKeyMake, 0x11), key)
              == ScanNormalizer::Result::Press);
    QNP_CHECK(key.scanCode == 0x1D);
    QNP_CHECK(!key.extended);
    QNP_CHECK(!key.e1);

    QNP_CHECK(normalizer.feed(make(0x1D, kRawKeyE0, 0x11), key)
              == ScanNormalizer::Result::Press);
    QNP_CHECK(key.scanCode == 0x1D);
    QNP_CHECK(key.extended);

    QNP_CHECK(normalizer.feed(make(0x1D, kRawKeyE0 | kRawKeyBreak, 0x11), key)
              == ScanNormalizer::Result::Release);
    QNP_CHECK(key.extended);
    QNP_CHECK(normalizer.feed(make(0x1D, kRawKeyBreak, 0x11), key)
              == ScanNormalizer::Result::Release);
    QNP_CHECK(!key.extended);
    QNP_CHECK(!normalizer.anyDown());

    CapturedKey leftAlt{};
    CapturedKey rightAlt{};
    QNP_CHECK(normalizer.feed(make(0x38, kRawKeyMake, 0x12), leftAlt)
              == ScanNormalizer::Result::Press);
    QNP_CHECK(normalizer.feed(make(0x38, kRawKeyE0, 0x12), rightAlt)
              == ScanNormalizer::Result::Press);
    QNP_CHECK(leftAlt != rightAlt);
}

QNP_TEST(ScanNormalizerSeparatesKeypadAndMainRow) {
    ScanNormalizer normalizer;
    CapturedKey mainEnter{};
    CapturedKey keypadEnter{};

    QNP_REQUIRE(normalizer.feed(make(0x1C, kRawKeyMake, 0x0D), mainEnter)
                == ScanNormalizer::Result::Press);
    QNP_REQUIRE(normalizer.feed(make(0x1C, kRawKeyE0, 0x0D), keypadEnter)
                == ScanNormalizer::Result::Press);
    QNP_CHECK(mainEnter.scanCode == keypadEnter.scanCode);
    QNP_CHECK(!mainEnter.extended);
    QNP_CHECK(keypadEnter.extended);
    QNP_CHECK(mainEnter != keypadEnter);

    QNP_CHECK(normalizer.isDown(mainEnter));
    QNP_CHECK(normalizer.isDown(keypadEnter));
    CapturedKey released{};
    QNP_CHECK(normalizer.feed(make(0x1C, kRawKeyE0 | kRawKeyBreak, 0x0D),
                              released)
              == ScanNormalizer::Result::Release);
    QNP_CHECK(!normalizer.isDown(keypadEnter));
    QNP_CHECK(normalizer.isDown(mainEnter));

    CapturedKey keypad4{};
    CapturedKey leftArrow{};
    QNP_REQUIRE(normalizer.feed(make(0x4B, kRawKeyMake, 0x64), keypad4)
                == ScanNormalizer::Result::Press);
    QNP_REQUIRE(normalizer.feed(make(0x4B, kRawKeyE0, 0x25), leftArrow)
                == ScanNormalizer::Result::Press);
    QNP_CHECK(keypad4 != leftArrow);
}

QNP_TEST(ScanNormalizerResolvesE1Pause) {
    ScanNormalizer normalizer;
    CapturedKey pause{};
    QNP_REQUIRE(normalizer.feed(make(0x1D, kRawKeyE1, 0x13), pause)
                == ScanNormalizer::Result::Press);
    QNP_CHECK(pause.scanCode == 0x1D);
    QNP_CHECK(pause.e1);
    QNP_CHECK(!pause.extended);

    CapturedKey tail{};
    QNP_CHECK(normalizer.feed(make(0x45, kRawKeyMake, 0x13), tail)
              == ScanNormalizer::Result::Ignored);

    CapturedKey released{};
    QNP_CHECK(normalizer.feed(make(0x1D, kRawKeyE1 | kRawKeyBreak, 0x13),
                              released)
              == ScanNormalizer::Result::Release);
    QNP_CHECK(released.e1);

    CapturedKey numLock{};
    QNP_REQUIRE(normalizer.feed(make(0x45, kRawKeyMake, 0x90), numLock)
                == ScanNormalizer::Result::Press);
    QNP_CHECK(numLock.scanCode == 0x45);
    QNP_CHECK(!numLock.e1);
    QNP_CHECK(numLock != pause);

    ScanNormalizer other;
    CapturedKey bogus{};
    QNP_CHECK(other.feed(make(0x2A, kRawKeyE1, 0x10), bogus)
              == ScanNormalizer::Result::Ignored);
}

QNP_TEST(ScanNormalizerSuppressesAutoRepeat) {
    ScanNormalizer normalizer;
    CapturedKey key{};

    QNP_CHECK(normalizer.feed(make(0x2C, kRawKeyMake, 0x5A), key)
              == ScanNormalizer::Result::Press);
    for (int i = 0; i < 25; ++i) {
        QNP_CHECK(normalizer.feed(make(0x2C, kRawKeyMake, 0x5A), key)
                  == ScanNormalizer::Result::Repeat);
    }
    QNP_CHECK(normalizer.feed(make(0x2C, kRawKeyBreak, 0x5A), key)
              == ScanNormalizer::Result::Release);
    QNP_CHECK(normalizer.feed(make(0x2C, kRawKeyMake, 0x5A), key)
              == ScanNormalizer::Result::Press);

    ScanNormalizer fresh;
    QNP_CHECK(fresh.feed(make(0x2C, kRawKeyBreak, 0x5A), key)
              == ScanNormalizer::Result::Ignored);
    QNP_CHECK(!fresh.anyDown());

    ScanNormalizer stale;
    QNP_REQUIRE(stale.feed(make(0x2C, kRawKeyMake, 0x5A), key)
                == ScanNormalizer::Result::Press);
    QNP_CHECK(stale.anyDown());
    stale.reset();
    QNP_CHECK(!stale.anyDown());
    QNP_CHECK(stale.feed(make(0x2C, kRawKeyMake, 0x5A), key)
              == ScanNormalizer::Result::Press);
}

QNP_TEST(ScanNormalizerIgnoresSyntheticPackets) {
    ScanNormalizer normalizer;
    CapturedKey key{};

    QNP_CHECK(normalizer.feed(make(0x2A, kRawKeyE0, 0x10), key)
              == ScanNormalizer::Result::Ignored);
    QNP_CHECK(normalizer.feed(make(0x36, kRawKeyE0, 0x10), key)
              == ScanNormalizer::Result::Ignored);
    QNP_CHECK(normalizer.feed(make(0x37, kRawKeyE0, 0x2C), key)
              == ScanNormalizer::Result::Press);
    QNP_CHECK(key.scanCode == 0x37);
    QNP_CHECK(key.extended);

    QNP_CHECK(normalizer.feed(make(0x2C, kRawKeyMake, 0xFF), key)
              == ScanNormalizer::Result::Ignored);
    QNP_CHECK(normalizer.feed(make(0x00, kRawKeyMake, 0x5A), key)
              == ScanNormalizer::Result::Ignored);

    QNP_CHECK(normalizer.feed(make(0x2A, kRawKeyMake, 0x10), key)
              == ScanNormalizer::Result::Press);
    QNP_CHECK(key.scanCode == 0x2A);
    QNP_CHECK(!key.extended);
}

QNP_TEST(RendererPrefersAcceleratedAndRecordsOrder) {
    std::vector<RendererKind> order;
    const RendererSelection selection = selectRenderer(
        [&order](RendererKind kind, std::string& driver, std::string& failure)
            -> bool {
            order.push_back(kind);
            (void)failure;
            driver = "direct3d11";
            return kind == RendererKind::Accelerated;
        });

    QNP_REQUIRE(order.size() == 1);
    QNP_CHECK(order[0] == RendererKind::Accelerated);
    QNP_CHECK(selection.ok());
    QNP_CHECK(selection.kind == RendererKind::Accelerated);
    QNP_CHECK(selection.driver == "direct3d11");
    QNP_CHECK(selection.fallbackReason.empty());
    QNP_CHECK(selection.failureReason.empty());
    QNP_CHECK(!selection.usedSoftwareFallback());
}

QNP_TEST(RendererFallsBackToSoftwareAndSaysWhy) {
    std::vector<RendererKind> order;
    const RendererSelection selection = selectRenderer(
        [&order](RendererKind kind, std::string& driver, std::string& failure)
            -> bool {
            order.push_back(kind);
            if (kind == RendererKind::Accelerated) {
                failure = "direct3d11: no compatible GPU in this prefix";
                return false;
            }
            driver = "software";
            return true;
        });

    QNP_REQUIRE(order.size() == 2);
    QNP_CHECK(order[0] == RendererKind::Accelerated);
    QNP_CHECK(order[1] == RendererKind::Software);
    QNP_CHECK(selection.ok());
    QNP_CHECK(selection.usedSoftwareFallback());
    QNP_CHECK(selection.driver == "software");
    QNP_CHECK(!selection.fallbackReason.empty());
    QNP_CHECK(selection.fallbackReason.find("no compatible GPU")
              != std::string::npos);
    QNP_REQUIRE(selection.attempts.size() == 2);
    QNP_CHECK(!selection.attempts[0].succeeded);
    QNP_CHECK(!selection.attempts[0].failure.empty());
    QNP_CHECK(selection.attempts[1].succeeded);
}

QNP_TEST(RendererFailureReasonIsNeverEmpty) {
    const RendererSelection silent = selectRenderer(
        [](RendererKind, std::string&, std::string&) -> bool { return false; });
    QNP_CHECK(!silent.ok());
    QNP_CHECK(silent.kind == RendererKind::None);
    QNP_CHECK(!silent.failureReason.empty());
    QNP_REQUIRE(silent.attempts.size() == 2);
    for (const RendererAttempt& attempt : silent.attempts)
        QNP_CHECK(!attempt.failure.empty());

    const RendererSelection strict = selectRenderer(
        [](RendererKind, std::string&, std::string& failure) -> bool {
            failure = "forced failure";
            return false;
        },
        false);
    QNP_CHECK(!strict.ok());
    QNP_CHECK(!strict.failureReason.empty());
    QNP_CHECK(strict.failureReason.find("forced failure") != std::string::npos);
    QNP_CHECK(strict.attempts.size() == 1);

    const RendererSelection none = selectRenderer(RendererFactory{});
    QNP_CHECK(!none.ok());
    QNP_CHECK(!none.failureReason.empty());

    const RendererSelection anonymous = selectRenderer(
        [](RendererKind kind, std::string&, std::string&) -> bool {
            return kind == RendererKind::Accelerated;
        });
    QNP_CHECK(anonymous.ok());
    QNP_CHECK(!anonymous.driver.empty());
}
