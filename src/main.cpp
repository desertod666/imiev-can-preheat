#include <Arduino.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <driver/twai.h>

#include "preheat_protocol.h"

namespace {

Preheat::Sequence sequence;
bool driverInstalled = false;
bool transmitEnabled = false;
uint32_t lastReportAt = 0;
uint32_t received = 0;
uint32_t submitted = 0;
uint32_t rejected = 0;
uint32_t stopFrames = 0;
int lastCommand = -1;
char input[40] = {};
size_t inputLength = 0;
bool inputOverflow = false;

struct Sample {
    bool seen = false;
    uint32_t at = 0;
    uint8_t bytes[8] = {};
    uint8_t length = 0;

    void update(const twai_message_t& frame, uint32_t now) {
        seen = true;
        at = now;
        length = frame.data_length_code;
        memcpy(bytes, frame.data, length);
    }

    bool fresh(uint32_t now) const { return seen && now - at < 2000; }
};

Sample heater;
Sample charger;
Sample remote;
Sample key;
Sample gear;
Sample speed;
bool heaterCurrentObserved = false;
bool zeroAfterStopObserved = false;
uint32_t stopAt = 0;

bool configureCan(bool transmit) {
    if (driverInstalled) {
        twai_stop();
        if (twai_driver_uninstall() != ESP_OK) return false;
        driverInstalled = false;
    }
    transmitEnabled = false;
    twai_general_config_t general = TWAI_GENERAL_CONFIG_DEFAULT(
        gpio_num_t(PREHEAT_CAN_TX), gpio_num_t(PREHEAT_CAN_RX),
        transmit ? TWAI_MODE_NORMAL : TWAI_MODE_LISTEN_ONLY);
    general.tx_queue_len = 8;
    general.rx_queue_len = 128;
    twai_timing_config_t timing = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    if (twai_driver_install(&general, &timing, &filter) != ESP_OK) return false;
    driverInstalled = true;
    if (twai_start() != ESP_OK) return false;
    transmitEnabled = transmit;
    return true;
}

void finish(const char* reason) {
    sequence.abort();
    const bool listening = configureCan(false);
    Serial.printf("END reason=%s submitted=%lu rejected=%lu stop_frames=%lu ptc_current_observed=%d zero_after_stop_observed=%d listen_only=%d\n",
        reason, (unsigned long)submitted, (unsigned long)rejected, (unsigned long)stopFrames,
        heaterCurrentObserved, zeroAfterStopObserved, listening);
    if (!listening) Serial.println("ERROR CAN driver unavailable. Check wiring and restart the board.");
}

const char* observedHazard(uint32_t now) {
    if (key.fresh(now) && (key.bytes[0] & 3)) return "KEY_ACC_OR_ON";
    if (gear.fresh(now) && gear.bytes[0] && strchr("RNDBC", gear.bytes[0])) return "GEAR_OUT_OF_PARK";
    if (speed.fresh(now) && speed.bytes[1] > 0 && speed.bytes[1] != 255) return "VEHICLE_MOVING";
    return nullptr;
}

void readCan(uint32_t now) {
    if (!driverInstalled) return;
    twai_message_t frame = {};
    for (unsigned i = 0; i < 64 && twai_receive(&frame, 0) == ESP_OK; ++i) {
        ++received;
        if (frame.extd || frame.rtr || frame.data_length_code > 8) continue;
        switch (frame.identifier) {
            case Preheat::CAN_ID:
                remote.update(frame, now);
                if (sequence.busy()) {
                    // Yield to a real Remote ECU instead of competing for its CAN ID.
                    finish("OTHER_REMOTE_DETECTED");
                    return;
                }
                break;
            case 0x424:
                if (frame.data_length_code >= 1) key.update(frame, now);
                break;
            case 0x418:
                if (frame.data_length_code >= 1) gear.update(frame, now);
                break;
            case 0x412:
                if (frame.data_length_code >= 2) speed.update(frame, now);
                break;
            case 0x389:
                if (frame.data_length_code == 8) charger.update(frame, now);
                break;
            case 0x384:
                if (frame.data_length_code != 8) break;
                heater.update(frame, now);
                if (sequence.phase() == Preheat::Phase::Heating && sequence.startSubmitted() && frame.data[4]) {
                    if (!heaterCurrentObserved) Serial.println("PTC_CURRENT_OBSERVED");
                    heaterCurrentObserved = true;
                }
                if (stopFrames && uint32_t(now - stopAt) < 15000 && frame.data[4] == 0 &&
                    frame.data[0] == 0 && frame.data[1] == 0) {
                    zeroAfterStopObserved = true;
                }
                break;
        }
    }
    if (sequence.busy()) {
        if (const char* hazard = observedHazard(now)) finish(hazard);
    }
}

void report(uint32_t now) {
    twai_status_info_t status = {};
    if (!driverInstalled || twai_get_status_info(&status) != ESP_OK) {
        Serial.println("STATUS CAN_UNAVAILABLE");
        return;
    }
    Serial.printf("STATUS phase=%s can=%s rx=%lu tx_submitted=%lu tx_failed=%lu rx_missed=%lu rx_overrun=%lu bus_errors=%lu\n",
        Preheat::phaseName(sequence.phase()), transmitEnabled ? "NORMAL" : "LISTEN_ONLY",
        (unsigned long)received, (unsigned long)submitted, (unsigned long)status.tx_failed_count,
        (unsigned long)status.rx_missed_count, (unsigned long)status.rx_overrun_count,
        (unsigned long)status.bus_error_count);
    if (heater.fresh(now)) {
        Serial.printf("RX384 ptc_A=%.1f compressor_raw=%u inlet_C=%.1f outlet_C=%.1f age_ms=%lu\n",
            heater.bytes[4] * 0.1, (unsigned)((heater.bytes[0] << 8) | heater.bytes[1]),
            heater.bytes[5] * 0.6 - 40, heater.bytes[6] * 0.6 - 40, (unsigned long)(now - heater.at));
    } else {
        Serial.println("RX384 STALE_OR_MISSING");
    }
    if (charger.fresh(now)) {
        Serial.printf("RX389 ac_input_A=%.1f age_ms=%lu\n", charger.bytes[6] * 0.1, (unsigned long)(now - charger.at));
    }
}

void command(char* text, uint32_t now) {
    for (char* p = text; *p; ++p) *p = char(toupper(static_cast<unsigned char>(*p)));
    char* context = nullptr;
    const char* verb = strtok_r(text, " \t", &context);
    if (!verb) return;
    const char* argument = strtok_r(nullptr, " \t", &context);
    if (strtok_r(nullptr, " \t", &context)) {
        Serial.println("ERROR Too many arguments.");
        return;
    }
    if (!strcmp(verb, "HEAT") || !strcmp(verb, "DEFROST")) {
        char* end = nullptr;
        const unsigned long seconds = argument ? strtoul(argument, &end, 10) : 60;
        if (!seconds || seconds > Preheat::MAX_SECONDS || (argument && (end == argument || *end))) {
            Serial.println("ERROR Duration must be 1 to 3600 whole seconds.");
            return;
        }
        if (sequence.busy()) {
            Serial.println("ERROR A sequence is running. Use STOP and wait for IDLE.");
            return;
        }
        if (argument && (!*argument || argument[0] == '-' || *end || !seconds || seconds > Preheat::MAX_SECONDS)) {
            Serial.println("ERROR Duration must be 1..3600 seconds.");
            return;
        }
        if (sequence.busy()) { Serial.println("ERROR Already running. Use STOP first."); return; }
        if (remote.fresh(now)) { Serial.println("ERROR Another Remote ECU is sending 0x38D."); return; }
        if (const char* hazard = observedHazard(now)) { Serial.printf("ERROR %s\n", hazard); return; }
        if (!configureCan(true)) { finish("CAN_START_FAILED"); return; }
        const Preheat::Mode mode = !strcmp(verb, "HEAT") ? Preheat::Mode::Heat : Preheat::Mode::Defrost;
        submitted = rejected = stopFrames = 0;
        stopAt = 0;
        heaterCurrentObserved = zeroAfterStopObserved = false;
        lastCommand = -1;
        sequence.start(mode, uint16_t(seconds), now);
        Serial.printf("START_REQUEST mode=%s hold_seconds=%lu\n", verb, seconds);
    } else if (!strcmp(verb, "STOP") && !argument) {
        if (!sequence.busy()) {
            if (remote.fresh(now)) { Serial.println("ERROR Another Remote ECU is sending 0x38D."); return; }
            if (!configureCan(true)) { finish("CAN_START_FAILED"); return; }
            stopFrames = 0;
            zeroAfterStopObserved = false;
        }
        if (sequence.phase() != Preheat::Phase::Stopping && sequence.phase() != Preheat::Phase::Observing) {
            twai_clear_transmit_queue();
        }
        sequence.stop(now);
        Serial.println("STOP_REQUEST");
    } else if (!strcmp(verb, "STATUS") && !argument) {
        report(now);
    } else {
        Serial.println("Commands: HEAT [seconds], DEFROST [seconds], STOP, STATUS, HELP");
    }
}

void readSerial(uint32_t now) {
    for (unsigned count = 0; count < 64 && Serial.available(); ++count) {
        const char ch = char(Serial.read());
        if (ch == '\n' || ch == '\r') {
            if (inputOverflow) Serial.println("ERROR Command too long.");
            else if (inputLength) { input[inputLength] = 0; command(input, now); }
            inputLength = 0;
            inputOverflow = false;
        } else if (!inputOverflow) {
            if (inputLength + 1 < sizeof(input)) input[inputLength++] = ch;
            else inputOverflow = true;
        }
    }
}

void serviceSequence(uint32_t now) {
    if (!sequence.busy()) return;
    twai_status_info_t status = {};
    if (twai_get_status_info(&status) != ESP_OK || status.state != TWAI_STATE_RUNNING) {
        finish("CAN_NOT_RUNNING_STOP_UNCONFIRMED");
        return;
    }
    const Preheat::Phase before = sequence.phase();
    uint8_t value = 0;
    const bool send = sequence.next(now, value);
    if (sequence.phase() != before) {
        Serial.printf("PHASE %s\n", Preheat::phaseName(sequence.phase()));
        if (sequence.phase() == Preheat::Phase::Stopping) twai_clear_transmit_queue();
    }
    if (!sequence.busy()) {
        finish(stopFrames ? "STOP_SEQUENCE_COMPLETE_CHECK_FEEDBACK" : "NO_STOP_SUBMITTED");
        return;
    }
    if (!send) return;
    twai_message_t frame = {};
    frame.identifier = Preheat::CAN_ID;
    frame.data_length_code = 8;
    frame.data[0] = value;
    frame.ss = 1;
    if (twai_transmit(&frame, 0) == ESP_OK) {
        ++submitted;
        sequence.submitted(now);
        if (sequence.phase() == Preheat::Phase::Stopping) {
            if (!stopFrames) stopAt = now;
            ++stopFrames;
        }
        if (lastCommand != value) {
            Serial.printf("TX_SUBMITTED 38D %02X 00 00 00 00 00 00 00\n", value);
            lastCommand = value;
        }
    } else {
        ++rejected;
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    configureCan(false);
}

void loop() {
    const uint32_t now = millis();
    readCan(now);
    readSerial(now);
    serviceSequence(now);
    if (now - lastReportAt >= 1000) {
        lastReportAt = now;
        report(now);
    }
    delay(1);
}
