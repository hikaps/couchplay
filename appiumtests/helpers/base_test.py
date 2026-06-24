# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors

from appium.webdriver.common.appiumby import AppiumBy
from conftest import (
    DEFAULT_TIMEOUT,
    click_by_class_name,
    click_by_name,
    click_by_object_name,
    navigate_to_page,
    open_global_drawer,
    wait_for_element,
    wait_for_element_clickable,
)


class BaseTest:
    def wait_for_element(self, driver, by, value, timeout=DEFAULT_TIMEOUT):
        return wait_for_element(driver, by, value, timeout)

    def wait_for_element_clickable(self, driver, by, value, timeout=DEFAULT_TIMEOUT):
        return wait_for_element_clickable(driver, by, value, timeout)

    def click_by_name(self, driver, name, timeout=DEFAULT_TIMEOUT):
        return click_by_name(driver, name, timeout)

    def click_by_object_name(self, driver, object_name, timeout=DEFAULT_TIMEOUT):
        return click_by_object_name(driver, object_name, timeout)

    def click_by_class_name(self, driver, class_name, timeout=DEFAULT_TIMEOUT):
        return click_by_class_name(driver, class_name, timeout)

    def open_global_drawer(self, driver):
        open_global_drawer(driver)

    def navigate_to_session_setup(self, driver):
        self.click_by_object_name(driver, "cardNewSession")
        # Assert a SessionSetup-only Item to confirm navigation (avoids the false
        # positive where NAME 'New Session' matches home card text).
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "spinPlayerCount")

    def navigate_to_profiles(self, driver):
        navigate_to_page(driver, "Profiles", "Profiles")

    def navigate_to_users(self, driver):
        navigate_to_page(driver, "Users", "Users")

    def navigate_to_settings(self, driver):
        navigate_to_page(driver, "Settings", "Settings")

    def navigate_to_device_assignment(self, driver):
        self.navigate_to_session_setup(driver)
        self.click_by_name(driver, "Assign Devices")
        self.wait_for_element(driver, AppiumBy.NAME, "Assign Devices")
