// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps

#include <QtTest>

extern "C" {
#include "../tools/padfwd/padmap.h"
}

#include <cstdlib>
#include <cstring>

class TestPadMap : public QObject
{
    Q_OBJECT

private:
    static void clearRanges(padmap_range (&r)[ABS_CNT])
    {
        std::memset(r, 0, sizeof(r));
    }

private Q_SLOTS:
    void rescaleTriggerToStick()
    {
        const padmap_range src{0, 255};
        const padmap_range dst{-32767, 32767};
        QCOMPARE(padmap_rescale(0, src, dst), -32767);
        QCOMPARE(padmap_rescale(255, src, dst), 32767);
        QVERIFY(std::abs(padmap_rescale(127, src, dst)) <= 258);
        QVERIFY(std::abs(padmap_rescale(128, src, dst)) <= 258);
    }

    void rescaleStickToTrigger()
    {
        const padmap_range src{-32767, 32767};
        const padmap_range dst{0, 255};
        QCOMPARE(padmap_rescale(-32767, src, dst), 0);
        QCOMPARE(padmap_rescale(32767, src, dst), 255);
        QVERIFY(std::abs(padmap_rescale(0, src, dst) - 127) <= 1);
    }

    void rescaleClampsAndDegenerate()
    {
        const padmap_range src{0, 255};
        const padmap_range dst{-32767, 32767};
        QCOMPARE(padmap_rescale(-10, src, dst), -32767);
        QCOMPARE(padmap_rescale(999, src, dst), 32767);
        QCOMPARE(padmap_rescale(5, padmap_range{7, 7}, dst), 0);
    }

    void absAxisRescaled()
    {
        padmap_range abs[ABS_CNT];
        clearRanges(abs);
        abs[ABS_X] = {0, 255};
        padmap_event out[2];
        QCOMPARE(padmap_translate(EV_ABS, ABS_X, 255, abs, out), 1);
        QCOMPARE(int(out[0].type), int(EV_ABS));
        QCOMPARE(int(out[0].code), int(ABS_X));
        QCOMPARE(out[0].value, 32767);
    }

    void unknownRangeIsIdentity()
    {
        padmap_range abs[ABS_CNT];
        clearRanges(abs);
        padmap_event out[2];
        QCOMPARE(padmap_translate(EV_ABS, ABS_RX, -1234, abs, out), 1);
        QCOMPARE(out[0].value, -1234);
    }

    void gasBrakeMapToTriggers()
    {
        padmap_range abs[ABS_CNT];
        clearRanges(abs);
        abs[ABS_GAS] = {0, 1023};
        abs[ABS_BRAKE] = {0, 1023};
        padmap_event out[2];
        QCOMPARE(padmap_translate(EV_ABS, ABS_GAS, 1023, abs, out), 1);
        QCOMPARE(int(out[0].code), int(ABS_RZ));
        QCOMPARE(out[0].value, 255);
        QCOMPARE(padmap_translate(EV_ABS, ABS_BRAKE, 1023, abs, out), 1);
        QCOMPARE(int(out[0].code), int(ABS_Z));
        QCOMPARE(out[0].value, 255);
        QCOMPARE(padmap_translate(EV_ABS, ABS_BRAKE, 0, abs, out), 1);
        QCOMPARE(out[0].value, 0);
    }

    void digitalTriggers()
    {
        padmap_range abs[ABS_CNT];
        clearRanges(abs);
        padmap_event out[2];
        QCOMPARE(padmap_translate(EV_KEY, BTN_TL2, 1, abs, out), 1);
        QCOMPARE(int(out[0].type), int(EV_ABS));
        QCOMPARE(int(out[0].code), int(ABS_Z));
        QCOMPARE(out[0].value, 255);
        QCOMPARE(padmap_translate(EV_KEY, BTN_TL2, 0, abs, out), 1);
        QCOMPARE(out[0].value, 0);
        QCOMPARE(padmap_translate(EV_KEY, BTN_TR2, 1, abs, out), 1);
        QCOMPARE(int(out[0].code), int(ABS_RZ));
        QCOMPARE(out[0].value, 255);
        QCOMPARE(padmap_translate(EV_KEY, BTN_TR2, 0, abs, out), 1);
        QCOMPARE(out[0].value, 0);
    }

    void dpadButtonsBecomeHat()
    {
        padmap_range abs[ABS_CNT];
        clearRanges(abs);
        padmap_event out[2];
        struct Case { uint16_t code; uint16_t axis; int press; };
        const Case cases[] = {
            {BTN_DPAD_UP, ABS_HAT0Y, -1},
            {BTN_DPAD_DOWN, ABS_HAT0Y, 1},
            {BTN_DPAD_LEFT, ABS_HAT0X, -1},
            {BTN_DPAD_RIGHT, ABS_HAT0X, 1},
        };
        for (const Case &c : cases) {
            QCOMPARE(padmap_translate(EV_KEY, c.code, 1, abs, out), 1);
            QCOMPARE(int(out[0].type), int(EV_ABS));
            QCOMPARE(int(out[0].code), int(c.axis));
            QCOMPARE(out[0].value, c.press);
            QCOMPARE(padmap_translate(EV_KEY, c.code, 0, abs, out), 1);
            QCOMPARE(int(out[0].code), int(c.axis));
            QCOMPARE(out[0].value, 0);
        }
    }

    void keysPassOrDrop()
    {
        padmap_range abs[ABS_CNT];
        clearRanges(abs);
        padmap_event out[2];
        QCOMPARE(padmap_translate(EV_KEY, BTN_SOUTH, 1, abs, out), 1);
        QCOMPARE(int(out[0].type), int(EV_KEY));
        QCOMPARE(int(out[0].code), int(BTN_A));
        QCOMPARE(out[0].value, 1);
        QCOMPARE(padmap_translate(EV_KEY, BTN_START, 0, abs, out), 1);
        QCOMPARE(padmap_translate(EV_KEY, BTN_TRIGGER_HAPPY1, 1, abs, out), 0);
        QCOMPARE(padmap_translate(EV_KEY, BTN_C, 1, abs, out), 0);
        QCOMPARE(padmap_translate(EV_KEY, BTN_A, 2, abs, out), 0);
        QCOMPARE(padmap_translate(EV_MSC, MSC_SCAN, 1, abs, out), 0);
        QCOMPARE(padmap_translate(EV_ABS, ABS_MISC, 1, abs, out), 0);
    }

    void synReportPassesThrough()
    {
        padmap_range abs[ABS_CNT];
        clearRanges(abs);
        padmap_event out[2];
        QCOMPARE(padmap_translate(EV_SYN, SYN_REPORT, 0, abs, out), 1);
        QCOMPARE(int(out[0].type), int(EV_SYN));
        QCOMPARE(padmap_translate(EV_SYN, SYN_DROPPED, 0, abs, out), 0);
    }

    void chordFiresOnceAfterHold()
    {
        padmap_chord c;
        padmap_chord_init(&c);
        QCOMPARE(padmap_chord_next_ms(&c, 0), -1);
        padmap_chord_update(&c, BTN_START, 1, 1000);
        QCOMPARE(padmap_chord_poll(&c, 4000), 0); // only one button held
        padmap_chord_update(&c, BTN_SELECT, 1, 1100);
        QCOMPARE(padmap_chord_next_ms(&c, 1100), 2000);
        QCOMPARE(padmap_chord_poll(&c, 3099), 0);
        QCOMPARE(padmap_chord_poll(&c, 3100), 1);
        QCOMPARE(padmap_chord_poll(&c, 5000), 0); // once per hold
        QCOMPARE(padmap_chord_next_ms(&c, 5000), -1);
    }

    void chordResetsOnRelease()
    {
        padmap_chord c;
        padmap_chord_init(&c);
        padmap_chord_update(&c, BTN_START, 1, 0);
        padmap_chord_update(&c, BTN_SELECT, 1, 0);
        padmap_chord_update(&c, BTN_SELECT, 0, 1500);
        QCOMPARE(padmap_chord_poll(&c, 2500), 0);
        padmap_chord_update(&c, BTN_SELECT, 1, 3000);
        QCOMPARE(padmap_chord_poll(&c, 4999), 0);
        QCOMPARE(padmap_chord_poll(&c, 5000), 1);
        // release and hold again: fires again
        padmap_chord_update(&c, BTN_START, 0, 5100);
        padmap_chord_update(&c, BTN_START, 1, 6000);
        QCOMPARE(padmap_chord_poll(&c, 8000), 1);
    }
};

QTEST_APPLESS_MAIN(TestPadMap)
#include "test_padmap.moc"
