# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors

import time

from evdev import UInput, AbsInfo, ecodes


def create_virtual_gamepads(count=2):
    devices = []
    for i in range(count):
        device = UInput(
            {
                ecodes.EV_KEY: [
                    ecodes.BTN_SOUTH,
                    ecodes.BTN_EAST,
                    ecodes.BTN_NORTH,
                    ecodes.BTN_WEST,
                    ecodes.BTN_TL,
                    ecodes.BTN_TR,
                    ecodes.BTN_START,
                    ecodes.BTN_SELECT,
                    ecodes.BTN_MODE,
                    ecodes.BTN_THUMBL,
                    ecodes.BTN_THUMBR,
                ],
                ecodes.EV_ABS: [
                    (ecodes.ABS_X, AbsInfo(0, -32768, 32767, 16, 128, 0)),
                    (ecodes.ABS_Y, AbsInfo(0, -32768, 32767, 16, 128, 0)),
                    (ecodes.ABS_RX, AbsInfo(0, -32768, 32767, 16, 128, 0)),
                    (ecodes.ABS_RY, AbsInfo(0, -32768, 32767, 16, 128, 0)),
                    (ecodes.ABS_Z, AbsInfo(0, 0, 255, 0, 0, 0)),
                    (ecodes.ABS_RZ, AbsInfo(0, 0, 255, 0, 0, 0)),
                    (ecodes.ABS_HAT0X, AbsInfo(0, -1, 1, 0, 0, 0)),
                    (ecodes.ABS_HAT0Y, AbsInfo(0, -1, 1, 0, 0, 0)),
                ],
            },
            name=f"Virtual CouchPlay Gamepad {i}",
            vendor=0x045E,
            product=0x028E,
        )
        devices.append(device)
    time.sleep(0.5)
    return devices


def destroy_virtual_gamepads(devices):
    for device in devices:
        try:
            device.destroy()
        except Exception:
            pass
