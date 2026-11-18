#pragma once

#include <stdint.h>

namespace Preheat {

constexpr uint16_t CAN_ID = 0x38D;
constexpr uint32_t PERIOD_MS = 50;
constexpr uint32_t BASELINE_MS = 1500;
constexpr uint32_t NEUTRAL_MS = 3100;
constexpr uint32_t STOP_MS = 1500;
constexpr uint32_t OBSERVE_MS = 6000;
constexpr uint16_t MAX_SECONDS = 3600;

enum class Mode { Heat, Defrost };
enum class Phase { Idle, Baseline, Neutral, Arming, Heating, Stopping, Observing };

struct Profile {
    uint8_t start;
    uint8_t follow;
    uint8_t stop;
    uint32_t armMs;
    uint32_t followMs;
};

inline Profile profile(Mode mode) {
    if (mode == Mode::Defrost) return {0xC7, 0xCF, 0xC6, 14950, 2150};
    return {0xC5, 0, 0xC4, 10150, 0};
}

inline const char* phaseName(Phase phase) {
    switch (phase) {
        case Phase::Idle: return "IDLE";
        case Phase::Baseline: return "BASELINE";
        case Phase::Neutral: return "NEUTRAL";
        case Phase::Arming: return "ARMING";
        case Phase::Heating: return "HEATING_REQUEST";
        case Phase::Stopping: return "STOP_REQUEST";
        case Phase::Observing: return "OBSERVING_STOP";
    }
    return "UNKNOWN";
}

class Sequence {
public:
    bool start(Mode mode, uint16_t seconds, uint32_t now) {
        if (busy() || seconds == 0 || seconds > MAX_SECONDS) return false;
        mode_ = mode;
        holdMs_ = uint32_t(seconds) * 1000;
        startSubmitted_ = false;
        enter(Phase::Baseline, now);
        return true;
    }

    void stop(uint32_t now) {
        // Repeated STOP must not keep extending the cancellation window.
        if (phase_ != Phase::Stopping && phase_ != Phase::Observing) enter(Phase::Stopping, now);
    }

    void abort() { phase_ = Phase::Idle; }
    bool busy() const { return phase_ != Phase::Idle; }
    Phase phase() const { return phase_; }
    Mode mode() const { return mode_; }
    bool startSubmitted() const { return startSubmitted_; }

    bool next(uint32_t now, uint8_t& value) {
        const Profile p = profile(mode_);
        const uint32_t age = now - phaseAt_;
        switch (phase_) {
            case Phase::Baseline:
                if (age >= BASELINE_MS) enter(Phase::Neutral, now);
                break;
            case Phase::Neutral:
                if (age >= NEUTRAL_MS) enter(Phase::Arming, now);
                break;
            case Phase::Arming:
                if (age >= p.armMs) enter(Phase::Heating, now);
                break;
            case Phase::Heating:
                if (startSubmitted_ && now - startAt_ >= holdMs_) enter(Phase::Stopping, now);
                break;
            case Phase::Stopping:
                if (age >= STOP_MS) enter(Phase::Observing, now);
                break;
            case Phase::Observing:
                if (age >= OBSERVE_MS) enter(Phase::Idle, now);
                break;
            case Phase::Idle:
                return false;
        }
        if (phase_ == Phase::Baseline || phase_ == Phase::Observing || phase_ == Phase::Idle) return false;
        if (attempted_ && now - lastAttemptAt_ < PERIOD_MS) return false;
        lastAttemptAt_ = now;
        attempted_ = true;
        switch (phase_) {
            case Phase::Neutral: value = 0x02; break;
            case Phase::Arming: value = 0xC2; break;
            case Phase::Stopping: value = p.stop; break;
            default:
                value = p.start;
                if (p.follow && startSubmitted_ && now - startAt_ >= p.followMs) value = p.follow;
                break;
        }
        return true;
    }

    void submitted(uint32_t now) {
        if (phase_ == Phase::Heating && !startSubmitted_) {
            startSubmitted_ = true;
            startAt_ = now;
        }
    }

private:
    void enter(Phase phase, uint32_t now) {
        phase_ = phase;
        phaseAt_ = now;
        attempted_ = false;
    }

    Mode mode_ = Mode::Heat;
    Phase phase_ = Phase::Idle;
    uint32_t phaseAt_ = 0;
    uint32_t startAt_ = 0;
    uint32_t holdMs_ = 0;
    uint32_t lastAttemptAt_ = 0;
    bool attempted_ = false;
    bool startSubmitted_ = false;
};

}  // namespace Preheat
