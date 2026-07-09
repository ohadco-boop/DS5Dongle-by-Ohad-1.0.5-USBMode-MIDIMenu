#include "midi_pt.h"

#include <cstring>
#include <cstdlib>
#include "pico/time.h"
#include "tusb.h"

namespace {

// Right stick -> MA2 Pan/Tilt MIDI notes.
// Speed is selected automatically by stick distance from center.
// Configure these notes in grandMA2 MIDI Remote as CMD rows:
//   60/61/62/63 -> +/- 1  If Selection
//   64/65/66/67 -> +/- 3  If Selection
//   68/69/70/71 -> +/- 10 If Selection
constexpr uint8_t NOTE_PAN_LEFT[3]   = {60, 64, 68};
constexpr uint8_t NOTE_PAN_RIGHT[3]  = {61, 65, 69};
constexpr uint8_t NOTE_TILT_UP[3]    = {62, 66, 70};
constexpr uint8_t NOTE_TILT_DOWN[3]  = {63, 67, 71};

// Optional button notes, kept outside the 60-71 Pan/Tilt range.
constexpr uint8_t NOTE_CROSS     = 80;
constexpr uint8_t NOTE_CIRCLE    = 81;
constexpr uint8_t NOTE_SQUARE    = 82;
constexpr uint8_t NOTE_TRIANGLE  = 83;
constexpr uint8_t NOTE_L1        = 84;
constexpr uint8_t NOTE_R1        = 85;
constexpr uint8_t NOTE_L2        = 86;
constexpr uint8_t NOTE_R2        = 87;
constexpr uint8_t NOTE_DPAD_UP   = 88;
constexpr uint8_t NOTE_DPAD_DOWN = 89;
constexpr uint8_t NOTE_DPAD_LEFT = 90;
constexpr uint8_t NOTE_DPAD_RIGHT= 91;
constexpr uint8_t NOTE_CREATE    = 92;
constexpr uint8_t NOTE_OPTIONS   = 93;
constexpr uint8_t NOTE_TOUCHPAD  = 94;
constexpr uint8_t NOTE_PS        = 95;

// 8-bit stick range is roughly center 128 and +/-127 travel.
// Deadzone is intentionally small for Hall/TMR/K-Silver-style sticks.
constexpr int DEAD_ON  = 6;   // enter Speed 1 above ~5%
constexpr int DEAD_OFF = 4;   // return to deadzone below ~3%
constexpr int S2_ON    = 47;  // enter Speed 2 above ~37%
constexpr int S2_OFF   = 41;  // return to Speed 1 below ~32%
constexpr int S3_ON    = 91;  // enter Speed 3 above ~72%
constexpr int S3_OFF   = 84;  // return to Speed 2 below ~66%

constexpr uint32_t REPEAT_US[3] = {
    120000u, // Speed 1: slow/precise
     70000u, // Speed 2: medium
     35000u  // Speed 3: fast
};

struct AxisState {
    int speed = 0;      // 0=off, 1..3 active
    int dir = 0;        // -1 / +1
    uint32_t next_us = 0;
};

uint8_t g_report[63]{};
bool g_have_report = false;
AxisState g_pan;
AxisState g_tilt;
uint8_t g_prev_face = 0;
uint8_t g_prev_btn8 = 0;
uint8_t g_prev_btn9 = 0;
uint8_t g_prev_dpad = 8;

static inline bool midi_ready() {
    return tud_midi_mounted();
}

void send_note_pulse(uint8_t note) {
    if (!midi_ready()) return;

    const uint8_t on[3]  = {0x90, note, 127}; // Channel 1 Note On
    const uint8_t off[3] = {0x80, note, 0};   // Channel 1 Note Off
    tud_midi_stream_write(0, on, sizeof(on));
    tud_midi_stream_write(0, off, sizeof(off));
}

int update_speed_with_hysteresis(int prev_speed, int mag) {
    switch (prev_speed) {
        case 0:
            return (mag >= DEAD_ON) ? 1 : 0;
        case 1:
            if (mag < DEAD_OFF) return 0;
            if (mag >= S2_ON) return 2;
            return 1;
        case 2:
            if (mag < DEAD_OFF) return 0;
            if (mag < S2_OFF) return 1;
            if (mag >= S3_ON) return 3;
            return 2;
        case 3:
        default:
            if (mag < DEAD_OFF) return 0;
            if (mag < S2_OFF) return 1;
            if (mag < S3_OFF) return 2;
            return 3;
    }
}

void process_axis(AxisState &axis, int raw_delta, const uint8_t *neg_notes, const uint8_t *pos_notes) {
    const int mag = std::abs(raw_delta);
    int dir = 0;
    if (raw_delta < 0) dir = -1;
    else if (raw_delta > 0) dir = 1;

    // Direction flip: reset the speed state so the new direction fires immediately.
    int prev_speed = axis.speed;
    if (dir != 0 && axis.dir != 0 && dir != axis.dir) {
        prev_speed = 0;
        axis.next_us = 0;
    }

    const int new_speed = (dir == 0) ? 0 : update_speed_with_hysteresis(prev_speed, mag);
    if (new_speed == 0) {
        axis.speed = 0;
        axis.dir = 0;
        axis.next_us = 0;
        return;
    }

    const uint32_t now = time_us_32();
    const bool changed = (axis.speed != new_speed) || (axis.dir != dir);
    if (changed || axis.next_us == 0 || (int32_t)(now - axis.next_us) >= 0) {
        const uint8_t note = (dir < 0 ? neg_notes : pos_notes)[new_speed - 1];
        send_note_pulse(note);
        axis.next_us = now + REPEAT_US[new_speed - 1];
    }

    axis.speed = new_speed;
    axis.dir = dir;
}

void process_buttons(const uint8_t *r) {
    // DualSense report layout used by this firmware:
    // r[7] low nibble = D-pad, high nibble = Square/Cross/Circle/Triangle.
    // r[8] bits = L1/R1/L2/R2/Create/Options/L3/R3.
    // r[9] bits = PS/Touchpad/Mute...
    const uint8_t dpad = r[7] & 0x0F;
    const uint8_t face = (r[7] >> 4) & 0x0F;
    const uint8_t b8 = r[8];
    const uint8_t b9 = r[9];

    const uint8_t face_rise = (uint8_t)(face & ~g_prev_face);
    if (face_rise & 0x01) send_note_pulse(NOTE_SQUARE);
    if (face_rise & 0x02) send_note_pulse(NOTE_CROSS);
    if (face_rise & 0x04) send_note_pulse(NOTE_CIRCLE);
    if (face_rise & 0x08) send_note_pulse(NOTE_TRIANGLE);

    const uint8_t b8_rise = (uint8_t)(b8 & ~g_prev_btn8);
    if (b8_rise & 0x01) send_note_pulse(NOTE_L1);
    if (b8_rise & 0x02) send_note_pulse(NOTE_R1);
    if (b8_rise & 0x04) send_note_pulse(NOTE_L2);
    if (b8_rise & 0x08) send_note_pulse(NOTE_R2);
    if (b8_rise & 0x10) send_note_pulse(NOTE_CREATE);
    if (b8_rise & 0x20) send_note_pulse(NOTE_OPTIONS);

    const uint8_t b9_rise = (uint8_t)(b9 & ~g_prev_btn9);
    if (b9_rise & 0x01) send_note_pulse(NOTE_PS);
    if (b9_rise & 0x02) send_note_pulse(NOTE_TOUCHPAD);

    // D-pad: fire on entering a cardinal direction. Diagonals are ignored for now.
    if (dpad != g_prev_dpad) {
        if (dpad == 0) send_note_pulse(NOTE_DPAD_UP);
        else if (dpad == 2) send_note_pulse(NOTE_DPAD_RIGHT);
        else if (dpad == 4) send_note_pulse(NOTE_DPAD_DOWN);
        else if (dpad == 6) send_note_pulse(NOTE_DPAD_LEFT);
    }

    g_prev_face = face;
    g_prev_btn8 = b8;
    g_prev_btn9 = b9;
    g_prev_dpad = dpad;
}

} // namespace

void midi_pt_init() {
    g_have_report = false;
    g_pan = AxisState{};
    g_tilt = AxisState{};
    g_prev_face = 0;
    g_prev_btn8 = 0;
    g_prev_btn9 = 0;
    g_prev_dpad = 8;
}

void midi_pt_note_report(const uint8_t *report, uint16_t len) {
    if (!report || len < sizeof(g_report)) return;
    memcpy(g_report, report, sizeof(g_report));
    g_have_report = true;
}

void midi_pt_loop() {
    if (!g_have_report || !midi_ready()) return;

    uint8_t r[63];
    memcpy(r, g_report, sizeof(r));

    // Right stick: r[2] = RX, r[3] = RY.
    // Center is 128. Lower Y means stick up, so negative maps to Tilt Up.
    const int rx_delta = (int)r[2] - 128;
    const int ry_delta = (int)r[3] - 128;

    process_axis(g_pan,  rx_delta, NOTE_PAN_LEFT, NOTE_PAN_RIGHT);
    process_axis(g_tilt, ry_delta, NOTE_TILT_UP, NOTE_TILT_DOWN);
    process_buttons(r);
}

