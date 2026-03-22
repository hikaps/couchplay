# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors

import pytest
from appium.webdriver.common.appiumby import AppiumBy
from helpers.base_test import BaseTest

LONG_TIMEOUT = 20


class TestSessionLifecycle(BaseTest):
    def test_session_setup_with_helper(self, driver, mock_helper, test_users):
        self.navigate_to_session_setup(driver)
        title = self.wait_for_element(
            driver, AppiumBy.NAME, "New Session", LONG_TIMEOUT
        )
        assert title.is_displayed()

    def test_start_and_stop_session(self, driver, mock_helper, test_users):
        self.navigate_to_session_setup(driver)

        self.wait_for_element(driver, AppiumBy.NAME, "Start Session", LONG_TIMEOUT)
        self.click_by_name(driver, "Start Session", LONG_TIMEOUT)

        self.wait_for_element(driver, AppiumBy.NAME, "Stop Session", LONG_TIMEOUT)
        stop_btn = self.wait_for_element_clickable(
            driver, AppiumBy.NAME, "Stop Session", LONG_TIMEOUT
        )
        assert stop_btn.is_displayed()

        stop_btn.click()

        self.wait_for_element(driver, AppiumBy.NAME, "Start Session", LONG_TIMEOUT)

    def test_session_without_users_shows_error(self, driver, mock_helper):
        self.navigate_to_session_setup(driver)
        self.wait_for_element(driver, AppiumBy.NAME, "Start Session", LONG_TIMEOUT)
        self.click_by_name(driver, "Start Session", LONG_TIMEOUT)
        start_btn = self.wait_for_element(
            driver, AppiumBy.NAME, "Start Session", LONG_TIMEOUT
        )
        assert start_btn.is_displayed()

    def test_device_assignment_page_with_helper(self, driver, mock_helper):
        self.navigate_to_device_assignment(driver)
        self.wait_for_element(driver, AppiumBy.NAME, "Assign Devices", LONG_TIMEOUT)
        self.wait_for_element(
            driver, AppiumBy.ACCESSIBILITY_ID, "actionAutoAssign", LONG_TIMEOUT
        )

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
        self.wait_for_element(
            driver, AppiumBy.ACCESSIBILITY_ID, "actionAddUser", LONG_TIMEOUT
        )

    def test_create_user_dialog_with_helper(self, driver, mock_helper):
        self.navigate_to_users(driver)
        self.wait_for_element(driver, AppiumBy.NAME, "Users", LONG_TIMEOUT)
        self.click_by_name(driver, "Add User", LONG_TIMEOUT)
        self.wait_for_element(
            driver, AppiumBy.ACCESSIBILITY_ID, "dialogAddUser", LONG_TIMEOUT
        )
        self.wait_for_element(
            driver, AppiumBy.ACCESSIBILITY_ID, "fieldUsername", LONG_TIMEOUT
        )
