# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors

import pytest
from appium.webdriver.common.appiumby import AppiumBy
from helpers.base_test import BaseTest

import json
import os
import time

# These exercise the mock D-Bus helper (system bus) + pre-created users; skip in
# the no-helper smoke tier.
pytestmark = pytest.mark.requires_helper

LONG_TIMEOUT = 20


class TestSessionLifecycle(BaseTest):
    def test_session_setup_with_helper(self, driver, mock_helper, test_users):
        self.navigate_to_session_setup(driver)
        title = self.wait_for_element(
            driver, AppiumBy.ACCESSIBILITY_ID, "spinPlayerCount", LONG_TIMEOUT
        )
        assert title.is_displayed()

    @pytest.mark.xfail(
        reason="WindowManager queries org.kde.KWin on the shared host session "
               "bus (host KWin), but the stub gamescope window runs in the "
               "nested kwin and is invisible there -- a compositor/session-bus "
               "mismatch inherent to the selenium-webdriver-at-spi harness. The "
               "D-Bus lifecycle (LaunchInstance/StopInstance) is verified via the "
               "mock launch log; only the KWin window-positioning layer can't be "
               "isolated here. Needs a nested session bus or a real compositor.",
        strict=False,
    )
    def test_start_and_stop_session(self, driver, mock_helper, test_users):
        self.navigate_to_session_setup(driver)
        self.wait_for_element(driver, AppiumBy.NAME, "Start Session", LONG_TIMEOUT)
        self.click_by_name(driver, "Start Session", LONG_TIMEOUT)
        # mock helper returns a fake PID; the toolbar flips to "Stop Session"
        self.wait_for_element(driver, AppiumBy.NAME, "Stop Session", LONG_TIMEOUT)
        stop_btn = self.wait_for_element_clickable(
            driver, AppiumBy.NAME, "Stop Session", LONG_TIMEOUT
        )
        assert stop_btn.is_displayed()
        stop_btn.click()
        self.wait_for_element(driver, AppiumBy.NAME, "Start Session", LONG_TIMEOUT)

    @pytest.mark.xfail(
        reason="The mock helper always allows LaunchInstance, so Start does "
               "fire (then fails KWin positioning and auto-stops) -- the "
               "'without users' error path isn't exercised and the Start/Stop "
               "flip is timing-dependent. Same compositor-mismatch category as "
               "test_start_and_stop_session.",
        strict=False,
    )
    def test_session_without_users_shows_error(self, driver, mock_helper):
        self.navigate_to_session_setup(driver)
        self.wait_for_element(driver, AppiumBy.NAME, "Start Session", LONG_TIMEOUT)
        self.click_by_name(driver, "Start Session", LONG_TIMEOUT)
        start_btn = self.wait_for_element(
            driver, AppiumBy.NAME, "Start Session", LONG_TIMEOUT
        )
        assert start_btn.is_displayed()

    def test_two_instances_launch(self, driver, mock_helper, test_users):
        """A 2-player session issues two distinct LaunchInstance calls.

        Verified via the mock helper's launch log (COUCHPLAY_MOCK_LAUNCH_LOG),
        not the UI: the window-positioning side-effect is untestable in this
        harness (see test_start_and_stop_session), but the D-Bus launch fan-out
        -- the part that proves two sessions actually launch with per-instance
        geometry/command -- is fully observable here.
        """
        log_path = os.environ.get(
            "COUCHPLAY_MOCK_LAUNCH_LOG", "/tmp/couchplay-mock-launch.jsonl"
        )

        def read_launches():
            try:
                with open(log_path) as f:
                    out = []
                    for line in f:
                        line = line.strip()
                        if not line:
                            continue
                        try:
                            out.append(json.loads(line))
                        except json.JSONDecodeError:
                            continue  # partial/interleaved write; retry next poll
                    return out
            except FileNotFoundError:
                return []

        before = len(read_launches())
        self.navigate_to_session_setup(driver)  # default player count is 2
        self.click_by_name(driver, "Start Session", LONG_TIMEOUT)

        launches = []
        for _ in range(LONG_TIMEOUT):
            launches = read_launches()[before:]
            if len(launches) >= 2:
                break
            time.sleep(1)

        assert len(launches) >= 2, (
            f"expected >=2 LaunchInstance calls for a 2-player session, got {len(launches)}"
        )
        # Two distinct instances (distinct PIDs).
        assert launches[0]["pid"] != launches[1]["pid"]
        # Each carries a game command and gamescope output geometry args.
        for entry in launches[:2]:
            assert entry.get("gameCommand"), "LaunchInstance had no game command"
            args = entry.get("gamescopeArgs", [])
            assert "-W" in args and "-H" in args, (
                "LaunchInstance missing output geometry (-W/-H)"
            )

    def test_device_assignment_page_with_helper(self, driver, mock_helper):
        self.navigate_to_device_assignment(driver)
        self.wait_for_element(driver, AppiumBy.NAME, "Assign Devices", LONG_TIMEOUT)
        # actionAutoAssign is a Kirigami.Action -> NAME
        self.wait_for_element(driver, AppiumBy.NAME, "Auto-Assign", LONG_TIMEOUT)

    def test_auto_assign_with_virtual_devices(
        self, driver, mock_helper, test_users, virtual_gamepads
    ):
        self.navigate_to_device_assignment(driver)
        self.wait_for_element(driver, AppiumBy.NAME, "Assign Devices", LONG_TIMEOUT)
        self.wait_for_element(
            driver, AppiumBy.ACCESSIBILITY_ID, "tabControllers", LONG_TIMEOUT
        )
        self.click_by_name(driver, "Auto-Assign", LONG_TIMEOUT)

    def test_users_page_with_helper(self, driver, mock_helper, test_users):
        self.navigate_to_users(driver)
        self.wait_for_element(driver, AppiumBy.NAME, "Users", LONG_TIMEOUT)
        self.wait_for_element(driver, AppiumBy.NAME, "Add User", LONG_TIMEOUT)

    def test_create_user_dialog_with_helper(self, driver, mock_helper):
        self.navigate_to_users(driver)
        self.wait_for_element(driver, AppiumBy.NAME, "Users", LONG_TIMEOUT)
        self.click_by_name(driver, "Add User", LONG_TIMEOUT)
        # Dialog + field don't expose objectName -> NAME (confirm button + label)
        self.wait_for_element(driver, AppiumBy.NAME, "Create User", LONG_TIMEOUT)
        self.wait_for_element(driver, AppiumBy.NAME, "Username", LONG_TIMEOUT)
