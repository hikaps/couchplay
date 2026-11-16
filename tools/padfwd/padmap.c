// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps

#include "padmap.h"

#include <string.h>

#define STICK_MAX 32767

int32_t padmap_rescale(int32_t v, padmap_range src, padmap_range dst)
{
    if (src.max == src.min) {
        return dst.min + (int32_t)(((int64_t)dst.max - dst.min) / 2);
    }
    int64_t smin = src.min, smax = src.max;
    int64_t den = smax - smin;
    int64_t x = v;
    if (den < 0) { // inverted source range
        smin = src.max;
        smax = src.min;
        den = -den;
    }
    if (x < smin) x = smin;
    if (x > smax) x = smax;
    int64_t num = (x - smin) * ((int64_t)dst.max - dst.min);
    int64_t q = num >= 0 ? (num + den / 2) / den : -((-num + den / 2) / den);
    return (int32_t)(dst.min + q);
}

int padmap_target_range(uint16_t code, padmap_range *range)
{
    switch (code) {
    case ABS_X:
    case ABS_Y:
    case ABS_RX:
    case ABS_RY:
        range->min = -STICK_MAX;
        range->max = STICK_MAX;
        return 1;
    case ABS_Z:
    case ABS_RZ:
        range->min = 0;
        range->max = 255;
        return 1;
    case ABS_HAT0X:
    case ABS_HAT0Y:
        range->min = -1;
        range->max = 1;
        return 1;
    default:
        return 0;
    }
}

int padmap_is_target_key(uint16_t code)
{
    switch (code) {
    case BTN_A:
    case BTN_B:
    case BTN_X:
    case BTN_Y:
    case BTN_TL:
    case BTN_TR:
    case BTN_SELECT:
    case BTN_START:
    case BTN_MODE:
    case BTN_THUMBL:
    case BTN_THUMBR:
        return 1;
    default:
        return 0;
    }
}

static int emit(padmap_event out[2], int n, uint16_t type, uint16_t code, int32_t value)
{
    out[n].type = type;
    out[n].code = code;
    out[n].value = value;
    return n + 1;
}

int padmap_translate(uint16_t type, uint16_t code, int32_t value,
                     const padmap_range src_abs[ABS_CNT], padmap_event out[2])
{
    if (type == EV_SYN) {
        if (code == SYN_REPORT) {
            return emit(out, 0, EV_SYN, SYN_REPORT, 0);
        }
        return 0;
    }

    if (type == EV_ABS) {
        if (code >= ABS_CNT) {
            return 0;
        }
        uint16_t target = code;
        if (code == ABS_GAS) {
            target = ABS_RZ;
        } else if (code == ABS_BRAKE) {
            target = ABS_Z;
        }
        padmap_range dst;
        if (!padmap_target_range(target, &dst)) {
            return 0;
        }
        padmap_range src = src_abs[code];
        if (src.min == 0 && src.max == 0) {
            src = dst; // unknown source range: assume identity
        }
        return emit(out, 0, EV_ABS, target, padmap_rescale(value, src, dst));
    }

    if (type == EV_KEY) {
        if (value == 2) { // autorepeat
            return 0;
        }
        switch (code) {
        case BTN_TL2: // pads with an analog trigger report both; the axis already carries it
            if (src_abs[ABS_Z].max != src_abs[ABS_Z].min || src_abs[ABS_BRAKE].max != src_abs[ABS_BRAKE].min) {
                return 0;
            }
            return emit(out, 0, EV_ABS, ABS_Z, value ? 255 : 0);
        case BTN_TR2:
            if (src_abs[ABS_RZ].max != src_abs[ABS_RZ].min || src_abs[ABS_GAS].max != src_abs[ABS_GAS].min) {
                return 0;
            }
            return emit(out, 0, EV_ABS, ABS_RZ, value ? 255 : 0);
        case BTN_DPAD_UP:
            return emit(out, 0, EV_ABS, ABS_HAT0Y, value ? -1 : 0);
        case BTN_DPAD_DOWN:
            return emit(out, 0, EV_ABS, ABS_HAT0Y, value ? 1 : 0);
        case BTN_DPAD_LEFT:
            return emit(out, 0, EV_ABS, ABS_HAT0X, value ? -1 : 0);
        case BTN_DPAD_RIGHT:
            return emit(out, 0, EV_ABS, ABS_HAT0X, value ? 1 : 0);
        default:
            break;
        }
        if (padmap_is_target_key(code)) {
            return emit(out, 0, EV_KEY, code, value ? 1 : 0);
        }
        return 0;
    }

    return 0;
}

void padmap_chord_init(padmap_chord *c)
{
    memset(c, 0, sizeof(*c));
    c->since_ms = -1;
}

void padmap_chord_update(padmap_chord *c, uint16_t code, int32_t value, int64_t now_ms)
{
    if (code == BTN_START) {
        c->start_held = value != 0;
    } else if (code == BTN_SELECT) {
        c->select_held = value != 0;
    } else {
        return;
    }
    if (c->start_held && c->select_held) {
        if (c->since_ms < 0) {
            c->since_ms = now_ms;
            c->fired = 0;
        }
    } else {
        c->since_ms = -1;
        c->fired = 0;
    }
}

int padmap_chord_poll(padmap_chord *c, int64_t now_ms)
{
    if (c->since_ms >= 0 && !c->fired && now_ms - c->since_ms >= PADMAP_CHORD_HOLD_MS) {
        c->fired = 1;
        return 1;
    }
    return 0;
}

int padmap_chord_next_ms(const padmap_chord *c, int64_t now_ms)
{
    if (c->since_ms < 0 || c->fired) {
        return -1;
    }
    int64_t left = c->since_ms + PADMAP_CHORD_HOLD_MS - now_ms;
    return left > 0 ? (int)left : 0;
}
