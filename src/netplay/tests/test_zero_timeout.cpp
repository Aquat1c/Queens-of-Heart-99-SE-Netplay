#include "TestHarness.h"

#include "../../client/ConsoleUi.h"
#include "../../net/Protocol.h"
#include "../../net/Sockets.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool makeTcpPair(qnet::TcpListener& listener,
                 qnet::TcpConn& client,
                 qnet::TcpConn& server) {
    uint16_t port = 0;
    for (port = 43400; port < 43440; ++port) {
        if (listener.listen(port)) break;
    }
    if (!listener.isOpen()
        || !client.connectTo("127.0.0.1", port, 2000)) {
        return false;
    }
    server = listener.acceptOne(2000);
    return server.isOpen();
}

bool waitReadable(SOCKET socket, int timeoutMs) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socket, &readSet);
    timeval timeout{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
    return select(0, &readSet, nullptr, nullptr, &timeout) == 1;
}

struct FakeConsoleInput {
    DWORD now = 100;
    std::vector<INPUT_RECORD> records;
    size_t next = 0;
    std::vector<DWORD> waits;

    static DWORD nowMs(void* opaque) {
        return static_cast<FakeConsoleInput*>(opaque)->now;
    }

    static DWORD wait(void* opaque, DWORD timeoutMs) {
        auto& self = *static_cast<FakeConsoleInput*>(opaque);
        self.waits.push_back(timeoutMs);
        return self.next < self.records.size()
            ? WAIT_OBJECT_0
            : WAIT_TIMEOUT;
    }

    static bool read(void* opaque, INPUT_RECORD& record) {
        auto& self = *static_cast<FakeConsoleInput*>(opaque);
        if (self.next >= self.records.size()) return false;
        record = self.records[self.next++];
        return true;
    }

    qui::detail::PollKeyOps ops() {
        return qui::detail::PollKeyOps{
            this,
            &FakeConsoleInput::nowMs,
            &FakeConsoleInput::wait,
            &FakeConsoleInput::read,
        };
    }
};

INPUT_RECORD mouseRecord() {
    INPUT_RECORD record{};
    record.EventType = MOUSE_EVENT;
    return record;
}

INPUT_RECORD keyRecord(WORD key) {
    INPUT_RECORD record{};
    record.EventType = KEY_EVENT;
    record.Event.KeyEvent.bKeyDown = TRUE;
    record.Event.KeyEvent.wVirtualKeyCode = key;
    return record;
}

struct FakeClipboard {
    enum Step {
        Allocate,
        Lock,
        Unlock,
        Open,
        Empty,
        Set,
        Close,
        Release,
    };

    bool allocationSucceeds = true;
    bool openSucceeds = true;
    bool emptySucceeds = true;
    bool setSucceeds = true;
    HWND openedOwner = nullptr;
    char memory[64] = {};
    std::vector<Step> steps;

    static HGLOBAL allocate(void* opaque, SIZE_T bytes) {
        auto& self = *static_cast<FakeClipboard*>(opaque);
        self.steps.push_back(Allocate);
        if (!self.allocationSucceeds || bytes > sizeof(self.memory))
            return nullptr;
        return reinterpret_cast<HGLOBAL>(self.memory);
    }
    static void* lock(void* opaque, HGLOBAL memory) {
        auto& self = *static_cast<FakeClipboard*>(opaque);
        self.steps.push_back(Lock);
        return memory;
    }
    static void unlock(void* opaque, HGLOBAL) {
        static_cast<FakeClipboard*>(opaque)->steps.push_back(Unlock);
    }
    static void release(void* opaque, HGLOBAL) {
        static_cast<FakeClipboard*>(opaque)->steps.push_back(Release);
    }
    static bool open(void* opaque, HWND owner) {
        auto& self = *static_cast<FakeClipboard*>(opaque);
        self.steps.push_back(Open);
        self.openedOwner = owner;
        return self.openSucceeds;
    }
    static bool empty(void* opaque) {
        auto& self = *static_cast<FakeClipboard*>(opaque);
        self.steps.push_back(Empty);
        return self.emptySucceeds;
    }
    static HANDLE setText(void* opaque, HGLOBAL memory) {
        auto& self = *static_cast<FakeClipboard*>(opaque);
        self.steps.push_back(Set);
        return self.setSucceeds ? memory : nullptr;
    }
    static void close(void* opaque) {
        static_cast<FakeClipboard*>(opaque)->steps.push_back(Close);
    }

    qui::detail::ClipboardWriteOps ops() {
        return qui::detail::ClipboardWriteOps{
            this,
            &FakeClipboard::allocate,
            &FakeClipboard::lock,
            &FakeClipboard::unlock,
            &FakeClipboard::release,
            &FakeClipboard::open,
            &FakeClipboard::empty,
            &FakeClipboard::setText,
            &FakeClipboard::close,
        };
    }
};

}

QNP_TEST(TcpZeroTimeoutPollsQueuedFrame) {
    qnet::TcpListener listener;
    qnet::TcpConn client;
    qnet::TcpConn server;
    QNP_REQUIRE(makeTcpPair(listener, client, server));

    qnet::Ping ping{ 77, 1234 };
    QNP_CHECK(client.sendMsg(qnet::MSG_PING, &ping, sizeof(ping)));
    QNP_CHECK(waitReadable(server.raw(), 1000));

    qnet::TcpConn::Received received;
    const uint64_t start = qnet::nowUs();
    QNP_CHECK(server.recvMsgEx(received, 0) == qnet::TcpConn::Rs::Msg);
    QNP_CHECK(qnet::nowUs() - start < 100000);
    QNP_CHECK(received.type == qnet::MSG_PING);
    QNP_CHECK(received.body.size() == sizeof(qnet::Ping));
}

QNP_TEST(TcpZeroTimeoutEmptyReturnsImmediately) {
    qnet::TcpListener listener;
    qnet::TcpConn client;
    qnet::TcpConn server;
    QNP_REQUIRE(makeTcpPair(listener, client, server));

    qnet::TcpConn::Received received;
    const uint64_t start = qnet::nowUs();
    QNP_CHECK(server.recvMsgEx(received, 0) == qnet::TcpConn::Rs::Timeout);
    QNP_CHECK(qnet::nowUs() - start < 100000);
}

QNP_TEST(ConsoleZeroTimeoutChecksAndDrainsQueuedInput) {
    FakeConsoleInput input;
    input.records.push_back(mouseRecord());
    input.records.push_back(keyRecord(VK_ESCAPE));

    QNP_CHECK(qui::detail::pollKeyWithOps(input.ops(), 0) == VK_ESCAPE);
    QNP_CHECK(input.next == 2);
    QNP_REQUIRE(input.waits.size() == 2);
    QNP_CHECK(input.waits[0] == 0 && input.waits[1] == 0);
}

QNP_TEST(ConsoleZeroTimeoutEmptyPollsExactlyOnce) {
    FakeConsoleInput input;
    QNP_CHECK(qui::detail::pollKeyWithOps(input.ops(), 0) == 0);
    QNP_REQUIRE(input.waits.size() == 1);
    QNP_CHECK(input.waits[0] == 0);
}

QNP_TEST(ConsoleClipboardRequiresOwnerAndPreparesBeforeEmptying) {
    FakeClipboard clipboard;
    const HWND owner = reinterpret_cast<HWND>(1);
    QNP_CHECK(qui::detail::copyTextWithOps(
        "203.0.113.40:7500", owner, clipboard.ops()));
    QNP_CHECK(clipboard.openedOwner == owner);
    QNP_CHECK(std::strcmp(clipboard.memory, "203.0.113.40:7500") == 0);
    const std::vector<FakeClipboard::Step> expected{
        FakeClipboard::Allocate,
        FakeClipboard::Lock,
        FakeClipboard::Unlock,
        FakeClipboard::Open,
        FakeClipboard::Empty,
        FakeClipboard::Set,
        FakeClipboard::Close,
    };
    QNP_CHECK(clipboard.steps == expected);

    FakeClipboard noOwner;
    QNP_CHECK(!qui::detail::copyTextWithOps(
        "unchanged", nullptr, noOwner.ops()));
    QNP_CHECK(noOwner.steps.empty());

    FakeClipboard allocationFailure;
    allocationFailure.allocationSucceeds = false;
    QNP_CHECK(!qui::detail::copyTextWithOps(
        "unchanged", owner, allocationFailure.ops()));
    QNP_REQUIRE(allocationFailure.steps.size() == 1);
    QNP_CHECK(allocationFailure.steps[0] == FakeClipboard::Allocate);

    FakeClipboard openFailure;
    openFailure.openSucceeds = false;
    QNP_CHECK(!qui::detail::copyTextWithOps(
        "unchanged", owner, openFailure.ops()));
    QNP_CHECK(std::find(openFailure.steps.begin(), openFailure.steps.end(),
                        FakeClipboard::Empty) == openFailure.steps.end());
    QNP_REQUIRE(!openFailure.steps.empty());
    QNP_CHECK(openFailure.steps.back() == FakeClipboard::Release);
}

QNP_TEST(ConsolePromptPreservesRepresentableDbcsCharactersAtomically) {
    if (!IsValidCodePage(932)) return;

    qui::detail::PromptLineBuffer prompt(932, 3);
    prompt.appendKeyUnit(L'\u3042');
    QNP_REQUIRE(prompt.encoded().size() == 2);
    QNP_CHECK(
        static_cast<unsigned char>(prompt.encoded()[0]) == 0x82
        && static_cast<unsigned char>(prompt.encoded()[1]) == 0xA0);

    prompt.appendKeyUnit(L'A');
    QNP_CHECK(prompt.encoded().size() == 3);
    prompt.appendKeyUnit(L'B');
    QNP_CHECK(prompt.encoded().size() == 3);

    prompt.backspace();
    QNP_CHECK(prompt.encoded().size() == 2);
    prompt.backspace();
    QNP_CHECK(prompt.encoded().empty());

    qui::detail::PromptLineBuffer cannotSplit(932, 1);
    cannotSplit.appendKeyUnit(L'\u3042');
    QNP_CHECK(cannotSplit.encoded().empty());

    std::wstring oversizedPaste(511, L'A');
    oversizedPaste += L'\u3042';
    oversizedPaste += L'B';
    qui::detail::PromptLineBuffer defaultBound(932);
    defaultBound.appendPastedText(oversizedPaste);
    QNP_CHECK(defaultBound.encoded().size() == 511);
    QNP_CHECK(defaultBound.encoded() == std::string(511, 'A'));
}

QNP_TEST(ConsolePromptAcceptsCyrillicWhenCodePageSupportsIt) {
    if (!IsValidCodePage(1251)) return;

    qui::detail::PromptLineBuffer prompt(1251);
    prompt.appendPastedText(L"\u0416\u044F");
    QNP_REQUIRE(prompt.encoded().size() == 2);
    QNP_CHECK(
        static_cast<unsigned char>(prompt.encoded()[0]) == 0xC6
        && static_cast<unsigned char>(prompt.encoded()[1]) == 0xFF);

    qui::detail::PromptLineBuffer western(1252);
    western.appendPastedText(L"A\u4E2DB");
    QNP_CHECK(western.encoded() == "AB");
}

QNP_TEST(ConsolePromptCombinesSurrogatesAndBackspacesOneScalar) {
    qui::detail::PromptLineBuffer prompt(CP_UTF8);
    prompt.appendKeyUnit(static_cast<wchar_t>(0xD83D));
    QNP_CHECK(prompt.hasPendingHighSurrogate());
    QNP_CHECK(prompt.encoded().empty());
    prompt.backspace();
    QNP_CHECK(!prompt.hasPendingHighSurrogate());
    QNP_CHECK(prompt.encoded().empty());

    prompt.appendKeyUnit(static_cast<wchar_t>(0xD83D));
    prompt.appendKeyUnit(static_cast<wchar_t>(0xDE00));
    QNP_CHECK(prompt.encoded() == "\xF0\x9F\x98\x80");
    prompt.appendKeyUnit(L'A');
    QNP_CHECK(prompt.encoded() == "\xF0\x9F\x98\x80"
                                      "A");
    prompt.backspace();
    QNP_CHECK(prompt.encoded() == "\xF0\x9F\x98\x80");
    prompt.backspace();
    QNP_CHECK(prompt.encoded().empty());
}

QNP_TEST(DisplayTextWrapsProseInsteadOfLosingItsTail) {
    const std::string title =
        "How should this controller be bound? Standardized names survive "
        "replugging. Binding by position uses the current backend's raw layout.";
    const std::vector<std::string> lines =
        qui::detail::wrapDisplayText(title, 40);
    QNP_CHECK(lines.size() > 1);
    std::string rejoined;
    for (const std::string& line : lines) {
        QNP_CHECK(qui::detail::prefixFittingColumns(line, 40) == line.size());
        QNP_CHECK(line.find("  ") == std::string::npos);
        if (!rejoined.empty()) rejoined += ' ';
        rejoined += line;
    }
    QNP_CHECK(rejoined == title);
}

QNP_TEST(DisplayTextHardBreaksAnUnbrokenTokenWithoutLosingBytes) {
    const std::string guid = "030057564c050000e60c000000016800";
    const std::vector<std::string> lines =
        qui::detail::wrapDisplayText(guid, 10);
    QNP_CHECK(lines.size() == 4);
    std::string rejoined;
    for (const std::string& line : lines) rejoined += line;
    QNP_CHECK(rejoined == guid);
}

QNP_TEST(DisplayTextWrapsMultiByteTextOnCharacterBoundaries) {
    const std::string japanese = "\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86";
    const std::vector<std::string> lines =
        qui::detail::wrapDisplayText(japanese, 4);
    QNP_CHECK(lines.size() == 2);
    QNP_CHECK(lines[0] == "\xE3\x81\x82\xE3\x81\x84");
    QNP_CHECK(lines[1] == "\xE3\x81\x86");
    std::string rejoined;
    for (const std::string& line : lines) rejoined += line;
    QNP_CHECK(rejoined == japanese);
}

QNP_TEST(ConsoleDisplayDecodesJapaneseAndCyrillicUtf8) {
    const std::string japanese("\xE3\x81\x82", 3);
    const std::string cyrillic("\xD0\x96", 2);
    QNP_CHECK(qui::detail::displayTextToWide(japanese) == L"\u3042");
    QNP_CHECK(qui::detail::displayTextToWide(cyrillic) == L"\u0416");
    QNP_CHECK(qui::detail::displayTextToWide("Player") == L"Player");
}

QNP_TEST(ConsolePromptPasteSanitizesControlsAndStopsAtNewline) {
    qui::detail::PromptLineBuffer prompt(CP_UTF8);
    prompt.appendPastedText(
        L"203.0.113.4:7500\t C:\\Game\\qoh99.exe"
        L"\x007F\x0085\r\nignored");
    QNP_CHECK(prompt.encoded()
              == "203.0.113.4:7500 C:\\Game\\qoh99.exe");

    qui::detail::PromptLineBuffer unicodeLineBreak(CP_UTF8);
    unicodeLineBreak.appendPastedText(L"first\u2028second");
    QNP_CHECK(unicodeLineBreak.encoded() == "first");
}
