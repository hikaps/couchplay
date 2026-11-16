// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps
//
// padmap: pure event mapping from arbitrary gamepads to the fixed Xbox 360
// layout exposed by couchplay-padfwd. No I/O; depends only on libc and
// <linux/input.h>, so it can be unit tested on its own.

#ifndef COUCHPLAY_PADMAP_H
#define COUCHPLAY_PADMAP_H

#include <stdint.h>
#include <linux/input.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PADMAP_CHORD_HOLD_MS 2000

typedef struct {
    int32_t min, max;
} padmap_range;

typedef struct {
    uint16_t type, code;
    int32_t value;
} padmap_event;

// Linear rescale of v from src to dst, clamped to src, rounded to nearest.
// If src.max == src.min the destination midpoint is returned.
int32_t padmap_rescale(int32_t v, padmap_range src, padmap_range dst);

// Fills *range with the Xbox 360 target range of an axis (sticks +-32767,
// triggers 0..255, hats -1..1). Returns 1 if the axis exists on the target,
// 0 otherwise.
int padmap_target_range(uint16_t code, padmap_range *range);

// 1 if code is one of the keys exposed on the target device.
int padmap_is_target_key(uint16_t code);

// Translates one source event into at most 2 target events. src_abs is
// indexed by source axis code; an all-zero entry means "unknown", in which
// case the value is assumed to already be in the target range.
// Returns the number of events written to out (0 = drop).
int padmap_translate(uint16_t type, uint16_t code, int32_t value,
                     const padmap_range src_abs[ABS_CNT], padmap_event out[2]);

// Exit chord (BTN_START + BTN_SELECT held together).
typedef struct {
    int start_held;
    int select_held;
    int64_t since_ms; // time both became held, -1 if not both held
    int fired;        // already reported for the current hold
} padmap_chord;

void padmap_chord_init(padmap_chord *c);
// Feed a BTN_START/BTN_SELECT state change (value != 0 means held). Other codes are ignored.
void padmap_chord_update(padmap_chord *c, uint16_t code, int32_t value, int64_t now_ms);
// Returns 1 exactly once per hold, when it has lasted PADMAP_CHORD_HOLD_MS.
int padmap_chord_poll(padmap_chord *c, int64_t now_ms);
// Milliseconds until padmap_chord_poll may fire, 0 if already due, -1 if no deadline pending.
int padmap_chord_next_ms(const padmap_chord *c, int64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
