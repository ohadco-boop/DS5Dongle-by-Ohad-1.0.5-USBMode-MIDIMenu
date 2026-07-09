// Button remapping. See remap.h. Flash-sector pattern mirrors slots.cpp;
// the apply logic + button set are ported from SundayMoments/DS5_Bridge,
// extended for OLED on-device editing (PS / Touchpad / Mute included).

#include "remap.h"
#include "config.h"

#include <cstring>
#include <cstdio>
#include <algorithm>

#include "hardware/flash.h"
#include "hardware/sync.h"

namespace {

// Source/target button indices. Keep in sync with kRemapNames in oled.cpp.
enum RemapButton : uint8_t {
    RemapL2, RemapL1, RemapCreate, RemapDpadUp, RemapDpadLeft, RemapDpadDown,
    RemapDpadRight, RemapL3, RemapR2, RemapR1, RemapOptions, RemapTriangle,
    RemapCircle, RemapCross, RemapSquare, RemapR3, RemapPS, RemapTouchpad,
    RemapMute, RemapButtonCount,
};
static_assert(RemapButtonCount == kRemapCount, "kRemapCount must match RemapButton");

// USBGetStateData byte 8: shoulders / sticks / system bits.
constexpr uint8_t kL1Bit = 0x01, kR1Bit = 0x02, kL2Bit = 0x04, kR2Bit = 0x08,
                  kCreateBit = 0x10, kOptionsBit = 0x20, kL3Bit = 0x40, kR3Bit = 0x80;
// USBGetStateData byte 7: D-pad hat (low nibble) + face buttons (high nibble).
constexpr uint8_t kSquareBit = 0x10, kCrossBit = 0x20, kCircleBit = 0x40,
                  kTriangleBit = 0x80, kDpadMask = 0x0F;
// USBGetStateData byte 9: PS / Touchpad click / Mute. Preserve bits 3..7.
constexpr uint8_t kPSBit = 0x01, kTouchpadBit = 0x02, kMuteBit = 0x04;
// D-pad hat values.
constexpr uint8_t kUp = 0, kUpRight = 1, kRight = 2, kDownRight = 3, kDown = 4,
                  kDownLeft = 5, kLeft = 6, kUpLeft = 7, kNeutral = 8;

constexpr uint32_t REMAP_MAGIC = 0x44533505u; // "DS5\x05"; v5 adds PicoMic local action target
constexpr uint32_t REMAP_FLASH_OFFSET = PICO_FLASH_SIZE_BYTES - 3u * FLASH_SECTOR_SIZE;

struct __attribute__((packed)) RemapData {
    uint32_t magic;
    uint8_t  table[kRemapCount];
};
static_assert(sizeof(RemapData) <= FLASH_PAGE_SIZE);
static_assert(REMAP_FLASH_OFFSET % FLASH_SECTOR_SIZE == 0);

RemapData g_remap{};
bool      g_active   = false; // false = identity → remap_apply() fast-returns
uint16_t  g_revision = 0;

const RemapData *flash_remap() {
    return reinterpret_cast<const RemapData *>(XIP_BASE + REMAP_FLASH_OFFSET);
}

void set_identity() {
    for (int i = 0; i < kRemapCount; i++) g_remap.table[i] = (uint8_t) i;
    // Ohad fixed44 default: the physical DualSense Mute button is a local
    // Pico BT Mic toggle, not a host Mute key. The user can still choose
    // target "Mute" manually from the OLED Remap screen to pass it to PC.
    g_remap.table[RemapMute] = kRemapTargetPicoMic;
}

void recompute_active() {
    g_active = false;
    for (int i = 0; i < kRemapCount; i++) {
        if (g_remap.table[i] != i) { g_active = true; return; }
    }
}

bool valid_table(const uint8_t *t) {
    for (int i = 0; i < kRemapCount; i++) {
        if (t[i] >= kRemapCount && t[i] != kRemapTargetPicoMic && t[i] != kRemapTargetOff) return false; // normal, PicoMic, or off
    }
    return true;
}

bool save_to_flash() {
    alignas(4) uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xff, sizeof(page));
    memcpy(page, &g_remap, sizeof(g_remap));

    const uint32_t interrupts = save_and_disable_interrupts();
    flash_range_erase(REMAP_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(REMAP_FLASH_OFFSET, page, sizeof(page));
    restore_interrupts(interrupts);

    RemapData verify{};
    memcpy(&verify, flash_remap(), sizeof(verify));
    if (memcmp(&verify, &g_remap, sizeof(g_remap)) == 0) {
        printf("[Remap] flash write verified\n");
        return true;
    }
    printf("[Remap] flash write VERIFY FAILED\n");
    return false;
}

bool dpad_has(uint8_t dir, RemapButton b) {
    switch (b) {
        case RemapDpadUp:    return dir == kUp || dir == kUpRight || dir == kUpLeft;
        case RemapDpadRight: return dir == kRight || dir == kUpRight || dir == kDownRight;
        case RemapDpadDown:  return dir == kDown || dir == kDownRight || dir == kDownLeft;
        case RemapDpadLeft:  return dir == kLeft || dir == kUpLeft || dir == kDownLeft;
        default:             return false;
    }
}

uint8_t dpad_from(bool up, bool right, bool down, bool left) {
    if (up && right && !down && !left) return kUpRight;
    if (right && down && !up && !left) return kDownRight;
    if (down && left && !up && !right) return kDownLeft;
    if (left && up && !right && !down) return kUpLeft;
    if (up && !down)    return kUp;
    if (right && !left) return kRight;
    if (down && !up)    return kDown;
    if (left && !right) return kLeft;
    return kNeutral;
}

void extract_sources(const uint8_t *report, bool src[kRemapCount]) {
    memset(src, 0, kRemapCount);
    const uint8_t dir = report[7] & kDpadMask;
    src[RemapL2]        = (report[8] & kL2Bit) != 0;
    src[RemapL1]        = (report[8] & kL1Bit) != 0;
    src[RemapCreate]    = (report[8] & kCreateBit) != 0;
    src[RemapDpadUp]    = dpad_has(dir, RemapDpadUp);
    src[RemapDpadLeft]  = dpad_has(dir, RemapDpadLeft);
    src[RemapDpadDown]  = dpad_has(dir, RemapDpadDown);
    src[RemapDpadRight] = dpad_has(dir, RemapDpadRight);
    src[RemapL3]        = (report[8] & kL3Bit) != 0;
    src[RemapR2]        = (report[8] & kR2Bit) != 0;
    src[RemapR1]        = (report[8] & kR1Bit) != 0;
    src[RemapOptions]   = (report[8] & kOptionsBit) != 0;
    src[RemapTriangle]  = (report[7] & kTriangleBit) != 0;
    src[RemapCircle]    = (report[7] & kCircleBit) != 0;
    src[RemapCross]     = (report[7] & kCrossBit) != 0;
    src[RemapSquare]    = (report[7] & kSquareBit) != 0;
    src[RemapR3]        = (report[8] & kR3Bit) != 0;
    src[RemapPS]        = (report[9] & kPSBit) != 0;
    src[RemapTouchpad]  = (report[9] & kTouchpadBit) != 0;
    src[RemapMute]      = (report[9] & kMuteBit) != 0;
}

} // namespace

void remap_load() {
    memcpy(&g_remap, flash_remap(), sizeof(g_remap));
    if (g_remap.magic != REMAP_MAGIC || !valid_table(g_remap.table)) {
        printf("[Remap] flash sector empty/invalid/old, defaulting to identity\n");
        g_remap.magic = REMAP_MAGIC;
        set_identity();
        save_to_flash();
    }
    recompute_active();
    printf("[Remap] loaded (active=%d)\n", g_active);
}

void remap_get(uint8_t out[kRemapCount]) { memcpy(out, g_remap.table, kRemapCount); }

uint16_t remap_revision() { return g_revision; }

bool remap_set(const uint8_t *table) {
    if (!valid_table(table)) return false;
    memcpy(g_remap.table, table, kRemapCount);
    g_remap.magic = REMAP_MAGIC;
    recompute_active();
    g_revision++;
    return save_to_flash();
}

bool remap_pico_mic_pressed(const uint8_t *report) {
    bool src[kRemapCount]{};
    extract_sources(report, src);

    // Dedicated Mic/Mute button stays a local Pico mic toggle by default even
    // when the Remap switch is OFF. Other buttons need Remap ON.
    if (g_remap.table[RemapMute] == kRemapTargetPicoMic && src[RemapMute]) return true;

    if (!get_config().remap_enable) return false;
    for (int i = 0; i < kRemapCount; i++) {
        if (i == RemapMute) continue;
        if (g_remap.table[i] == kRemapTargetPicoMic && src[i]) return true;
    }
    return false;
}

void remap_apply(uint8_t *report) {
    // New OLED Remap screen controls host remapping. The PicoMic target is a
    // local firmware action; if the dedicated Mute button is mapped to PicoMic,
    // suppress it from the host even when Remap is globally OFF.
    if (!get_config().remap_enable) {
        if (g_remap.table[RemapMute] == kRemapTargetPicoMic) report[9] &= (uint8_t)~kMuteBit;
        return;
    }
    if (!g_active) return;

    bool    src[kRemapCount]{};
    uint8_t src_analog[kRemapCount]{};
    extract_sources(report, src);

    for (int i = 0; i < kRemapCount; i++) src_analog[i] = src[i] ? 0xFF : 0;
    src_analog[RemapL2] = report[4]; // L2 analog
    src_analog[RemapR2] = report[5]; // R2 analog

    bool    tgt[kRemapCount]{};
    uint8_t tgt_analog[kRemapCount]{};
    for (int s = 0; s < kRemapCount; s++) {
        const uint8_t t = g_remap.table[s];
        if (t == kRemapTargetPicoMic || t == kRemapTargetOff || t >= kRemapCount) continue; // local/off = no host output
        if (src[s]) tgt[t] = true;
        tgt_analog[t] = std::max(tgt_analog[t], src_analog[s]); // OR digital / max analog
    }

    report[4] = tgt_analog[RemapL2];
    report[5] = tgt_analog[RemapR2];

    // Byte 7 is entirely D-pad + face (all 8 bits) — rebuild it.
    report[7] = dpad_from(tgt[RemapDpadUp], tgt[RemapDpadRight],
                          tgt[RemapDpadDown], tgt[RemapDpadLeft]);
    if (tgt[RemapSquare])   report[7] |= kSquareBit;
    if (tgt[RemapCross])    report[7] |= kCrossBit;
    if (tgt[RemapCircle])   report[7] |= kCircleBit;
    if (tgt[RemapTriangle]) report[7] |= kTriangleBit;

    // Byte 8 is entirely shoulders/sticks/Create/Options (all 8 bits) — rebuild it.
    report[8] = 0;
    if (tgt[RemapL1])      report[8] |= kL1Bit;
    if (tgt[RemapR1])      report[8] |= kR1Bit;
    if (tgt[RemapL2])      report[8] |= kL2Bit;
    if (tgt[RemapR2])      report[8] |= kR2Bit;
    if (tgt[RemapCreate])  report[8] |= kCreateBit;
    if (tgt[RemapOptions]) report[8] |= kOptionsBit;
    if (tgt[RemapL3])      report[8] |= kL3Bit;
    if (tgt[RemapR3])      report[8] |= kR3Bit;

    // Byte 9: rebuild PS/Touchpad/Mute only; preserve reserved/Edge bits 3..7.
    report[9] &= (uint8_t)~(kPSBit | kTouchpadBit | kMuteBit);
    if (tgt[RemapPS])       report[9] |= kPSBit;
    if (tgt[RemapTouchpad]) report[9] |= kTouchpadBit;
    if (tgt[RemapMute])     report[9] |= kMuteBit;
}
