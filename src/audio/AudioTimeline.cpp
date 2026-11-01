#include "audio/AudioTimeline.h"

namespace qaudio {

void AudioTimeline::normalize(FrameJournal& journal) {
    FrameJournal rewritten{};
    rewritten.epoch = journal.epoch;
    rewritten.layoutGeneration = journal.layoutGeneration;
    rewritten.frame = journal.frame;
    rewritten.valid = journal.valid;
    rewritten.overflowed = journal.overflowed;

    uint16_t i = 0;
    while (i < journal.commandCount) {
        const AudioCommand& current = journal.commands[i];
        bool folded = false;
        if (current.kind == AudioKind::Stop
            && i + 1u < journal.commandCount) {
            const AudioCommand& next = journal.commands[i + 1u];
            const bool nextIsPlayOnSameEndpoint =
                (next.kind == AudioKind::PlayDuplicate
                 || next.kind == AudioKind::PlayBase)
                && next.endpoint == current.endpoint;
            if (nextIsPlayOnSameEndpoint) {
                AudioCommand replace = next;
                replace.kind = AudioKind::Replace;
                if (next.loadOutcome == LoadOutcome::Absent) {
                    replace.sampleId = -1;
                }
                rewritten.commands[rewritten.commandCount] = replace;
                rewritten.commands[rewritten.commandCount].ordinal =
                    rewritten.commandCount;
                ++rewritten.commandCount;
                i = static_cast<uint16_t>(i + 2u);
                folded = true;
            }
        }
        if (!folded) {
            rewritten.commands[rewritten.commandCount] = current;
            rewritten.commands[rewritten.commandCount].ordinal =
                rewritten.commandCount;
            ++rewritten.commandCount;
            ++i;
        }
    }
    journal = rewritten;
}

}
