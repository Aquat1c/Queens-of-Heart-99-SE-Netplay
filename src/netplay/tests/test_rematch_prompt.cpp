#include "TestHarness.h"

#include "netplay/RematchPrompt.h"

using namespace qnetplay;

namespace {

wire::RematchChoicePayload remoteChoice(uint32_t epoch, uint32_t frame,
                                        uint8_t side, uint8_t choice,
                                        uint8_t ready) {
    wire::RematchChoicePayload payload{};
    payload.epoch = epoch;
    payload.promptFrame = frame;
    payload.side = side;
    payload.choice = choice;
    payload.ready = ready;
    return payload;
}

}

QNP_TEST(RematchPromptAgreementHappyPath) {
    RematchPrompt prompt;
    QNP_CHECK(!prompt.active());
    QNP_REQUIRE(prompt.begin(3, 40000, 0,
                             1000));
    QNP_CHECK(prompt.deciding());
    QNP_CHECK(!prompt.agreementReached());
    QNP_CHECK(!prompt.begin(3, 40001, 0, 1001));

    QNP_REQUIRE(prompt.latchLocalConfirm(true));
    QNP_CHECK(prompt.localReady());
    QNP_CHECK(prompt.localChoice() == wire::kRematchChoiceRematch);
    QNP_CHECK(!prompt.agreementReached());
    QNP_CHECK(!prompt.latchLocalConfirm(true));
    QNP_CHECK(!prompt.latchLocalConfirm(false));
    QNP_CHECK(prompt.localChoice() == wire::kRematchChoiceRematch);

    QNP_REQUIRE(prompt.acceptRemote(
        remoteChoice(3, 40000, 1, wire::kRematchChoiceRematch, 1)));
    QNP_CHECK(prompt.agreementReached());

    prompt.markCommitPending();
    QNP_CHECK(prompt.phase() == RematchPromptPhase::CommitPending);
    QNP_CHECK(!prompt.latchLocalConfirm(false));
    QNP_CHECK(prompt.localReady());
    QNP_CHECK(prompt.localChoice() == wire::kRematchChoiceRematch);

    prompt.markCommitted();
    QNP_CHECK(prompt.advancing());
    QNP_CHECK(!prompt.shouldSend(999999));
    prompt.reset();
    QNP_CHECK(!prompt.active());
}

QNP_TEST(RematchPromptStaleAdvertisementsAreDropped) {
    RematchPrompt prompt;
    QNP_REQUIRE(prompt.begin(5, 50000, 1, 0));
    QNP_CHECK(!prompt.acceptRemote(
        remoteChoice(4, 50000, 0, wire::kRematchChoiceRematch, 1)));
    QNP_CHECK(!prompt.acceptRemote(
        remoteChoice(5, 49000, 0, wire::kRematchChoiceRematch, 1)));
    QNP_CHECK(!prompt.acceptRemote(
        remoteChoice(5, 50000, 1, wire::kRematchChoiceRematch, 1)));
    QNP_CHECK(!prompt.remoteSeen());
    QNP_CHECK(!prompt.agreementReached());
    QNP_REQUIRE(prompt.acceptRemote(
        remoteChoice(5, 50000, 0, wire::kRematchChoiceRematch, 1)));
    QNP_CHECK(prompt.remoteReady());
    QNP_REQUIRE(prompt.acceptRemote(
        remoteChoice(5, 50000, 0, wire::kRematchChoiceUndecided, 0)));
    QNP_CHECK(!prompt.remoteReady());
    QNP_REQUIRE(prompt.acceptRemote(
        remoteChoice(5, 50000, 0, wire::kRematchChoiceLeave, 1)));
    QNP_REQUIRE(prompt.latchLocalConfirm(true));
    QNP_CHECK(!prompt.agreementReached());
}

QNP_TEST(RematchPromptConfirmAndLeaveSemantics) {
    RematchPrompt prompt;
    QNP_REQUIRE(prompt.begin(2, 30000, 0, 0));
    QNP_REQUIRE(prompt.latchLocalConfirm(false));
    QNP_CHECK(prompt.localChoice() == wire::kRematchChoiceLeave);
    QNP_CHECK(prompt.localReady());
    QNP_CHECK(!prompt.latchLocalConfirm(true));
    QNP_CHECK(prompt.localChoice() == wire::kRematchChoiceLeave);
    QNP_REQUIRE(prompt.acceptRemote(
        remoteChoice(2, 30000, 1, wire::kRematchChoiceRematch, 1)));
    QNP_CHECK(!prompt.agreementReached());

    RematchPrompt stranded;
    QNP_REQUIRE(stranded.begin(2, 31000, 0, 0));
    QNP_REQUIRE(stranded.latchLocalConfirm(true));
    QNP_CHECK(stranded.localChoice() == wire::kRematchChoiceRematch);
    QNP_REQUIRE(stranded.latchLocalLeave());
    QNP_CHECK(stranded.localChoice() == wire::kRematchChoiceLeave);
    QNP_CHECK(stranded.localReady());
    QNP_CHECK(!stranded.latchLocalConfirm(true));
    QNP_CHECK(stranded.localChoice() == wire::kRematchChoiceLeave);
}

QNP_TEST(RematchPromptDecisionTimeout) {
    RematchPrompt prompt;
    QNP_REQUIRE(prompt.begin(1, 20000, 0, 5000));
    QNP_CHECK(!prompt.decisionTimedOut(5000));
    QNP_CHECK(!prompt.decisionTimedOut(
        5000 + kRematchDecisionTimeoutMs - 1));
    QNP_CHECK(prompt.decisionTimedOut(5000 + kRematchDecisionTimeoutMs));
    QNP_REQUIRE(prompt.latchLocalConfirm(true));
    QNP_CHECK(prompt.decisionTimedOut(5000 + kRematchDecisionTimeoutMs));
    QNP_REQUIRE(prompt.acceptRemote(
        remoteChoice(1, 20000, 1, wire::kRematchChoiceRematch, 1)));
    QNP_CHECK(!prompt.decisionTimedOut(5000 + kRematchDecisionTimeoutMs));
    prompt.markCommitPending();
    QNP_CHECK(!prompt.decisionTimedOut(5000 + 10 * kRematchDecisionTimeoutMs));
}

QNP_TEST(RematchPromptSendCadence) {
    RematchPrompt prompt;
    QNP_REQUIRE(prompt.begin(1, 10000, 0, 1000));
    QNP_CHECK(prompt.shouldSend(1000));
    prompt.markSent(1000);
    QNP_CHECK(!prompt.shouldSend(1000 + kRematchChoiceResendMs - 1));
    QNP_CHECK(prompt.shouldSend(1000 + kRematchChoiceResendMs));
    QNP_REQUIRE(prompt.latchLocalConfirm(true));
    QNP_CHECK(prompt.shouldSend(1001));
    prompt.markSent(1001);
    QNP_CHECK(!prompt.shouldSend(1001 + kRematchChoiceReadyResendMs - 1));
    QNP_CHECK(prompt.shouldSend(1001 + kRematchChoiceReadyResendMs));
    const wire::RematchChoicePayload payload = prompt.localAdvertisement();
    QNP_CHECK(payload.epoch == 1);
    QNP_CHECK(payload.promptFrame == 10000);
    QNP_CHECK(payload.side == 0);
    QNP_CHECK(payload.choice == wire::kRematchChoiceRematch);
    QNP_CHECK(payload.ready == 1);
    prompt.markCommitPending();
    QNP_CHECK(prompt.shouldSend(999999));
    prompt.markCommitted();
    QNP_CHECK(!prompt.shouldSend(999999));
}
