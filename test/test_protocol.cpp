#include "preheat_protocol.h"

#include <assert.h>
#include <stdio.h>

using namespace Preheat;

static uint32_t prepare(Sequence& sequence, Mode mode, uint32_t now) {
    uint8_t value = 0;
    assert(sequence.start(mode, 60, now));
    assert(!sequence.next(now, value));
    now += BASELINE_MS - 1;
    assert(!sequence.next(now, value));
    ++now;
    assert(sequence.next(now, value) && value == 0x02);
    now += NEUTRAL_MS;
    assert(sequence.next(now, value) && value == 0xC2);
    now += profile(mode).armMs;
    assert(sequence.next(now, value) && value == profile(mode).start);
    sequence.submitted(now);
    return now;
}

static void heatRunsForTheWholeRequestedMinute(uint32_t start) {
    Sequence sequence;
    uint32_t now = prepare(sequence, Mode::Heat, start);
    uint8_t value = 0;
    for (uint32_t elapsed = PERIOD_MS; elapsed < 60000; elapsed += PERIOD_MS) {
        assert(sequence.next(now + elapsed, value) && value == 0xC5);
        sequence.submitted(now + elapsed);
    }
    now += 60000;
    assert(sequence.next(now, value) && value == 0xC4);
    assert(sequence.phase() == Phase::Stopping);
    now += STOP_MS;
    assert(!sequence.next(now, value));
    assert(sequence.phase() == Phase::Observing);
    assert(!sequence.next(now + OBSERVE_MS, value));
    assert(!sequence.busy());
}

static void defrostChangesToTheObservedFollowUpByte() {
    Sequence sequence;
    const uint32_t first = prepare(sequence, Mode::Defrost, 0);
    uint8_t value = 0;
    assert(!sequence.next(first + 49, value));
    assert(sequence.next(first + 2100, value) && value == 0xC7);
    assert(sequence.next(first + 2150, value) && value == 0xCF);
    sequence.stop(first + 2200);
    assert(sequence.next(first + 2200, value) && value == 0xC6);
    sequence.stop(first + 3000);
    assert(!sequence.next(first + 3700, value));
    assert(sequence.phase() == Phase::Observing);
}

static void failedSubmissionDoesNotStartTheHoldTimer() {
    Sequence sequence;
    uint8_t value = 0;
    assert(sequence.start(Mode::Heat, 1, 0));
    assert(sequence.next(BASELINE_MS, value));
    assert(sequence.next(BASELINE_MS + NEUTRAL_MS, value));
    const uint32_t heatAt = BASELINE_MS + NEUTRAL_MS + profile(Mode::Heat).armMs;
    assert(sequence.next(heatAt, value) && value == 0xC5);
    assert(!sequence.startSubmitted());
    assert(!sequence.next(heatAt + 1, value));
    assert(sequence.next(heatAt + 5000, value) && value == 0xC5);
    sequence.submitted(heatAt + 5000);
    assert(sequence.next(heatAt + 5950, value) && value == 0xC5);
    assert(sequence.next(heatAt + 6000, value) && value == 0xC4);
}

static void stopWorksDuringPreparationAndAfterRestart() {
    Sequence sequence;
    uint8_t value = 0;
    sequence.stop(0);
    assert(sequence.next(0, value) && value == 0xC4);
    sequence.abort();
    assert(!sequence.next(100, value));
    assert(sequence.start(Mode::Defrost, 1, 100));
    sequence.stop(101);
    assert(sequence.next(101, value) && value == 0xC6);
}

static void invalidAndOverlappingStartsAreRejected() {
    Sequence sequence;
    assert(!sequence.start(Mode::Heat, 0, 0));
    assert(!sequence.start(Mode::Heat, MAX_SECONDS + 1, 0));
    assert(sequence.start(Mode::Heat, MAX_SECONDS, 0));
    assert(!sequence.start(Mode::Defrost, 60, 0));
    assert(sequence.mode() == Mode::Heat);
}

int main() {
    heatRunsForTheWholeRequestedMinute(0);
    heatRunsForTheWholeRequestedMinute(UINT32_MAX - 1000);
    defrostChangesToTheObservedFollowUpByte();
    failedSubmissionDoesNotStartTheHoldTimer();
    stopWorksDuringPreparationAndAfterRestart();
    invalidAndOverlappingStartsAreRejected();
    puts("Protocol checks passed, including 32-bit timer wraparound.");
}
