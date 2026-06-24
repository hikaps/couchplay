# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors

from appium.webdriver.common.appiumby import AppiumBy
from helpers.base_test import BaseTest


class TestDeviceAssignment(BaseTest):
    def test_device_page_loads(self, driver):
        self.navigate_to_device_assignment(driver)
        title = self.wait_for_element(driver, AppiumBy.NAME, "Assign Devices")
        assert title.is_displayed()

    def test_toolbar_actions_present(self, driver):
        self.navigate_to_device_assignment(driver)
        # Toolbar actions are Kirigami.Action -> no objectName, use NAME
        self.wait_for_element(driver, AppiumBy.NAME, "Refresh")
        self.wait_for_element(driver, AppiumBy.NAME, "Auto-Assign")
        self.wait_for_element(driver, AppiumBy.NAME, "Clear All")

    def test_device_tabs_visible(self, driver):
        self.navigate_to_device_assignment(driver)
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "tabControllers")
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "tabKeyboards")
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "tabMice")

    def test_switch_device_tabs(self, driver):
        self.navigate_to_device_assignment(driver)
        self.click_by_object_name(driver, "tabKeyboards")
        self.click_by_object_name(driver, "tabMice")

    def test_player_count_spinbox(self, driver):
        self.navigate_to_device_assignment(driver)
        spin = self.wait_for_element(
            driver, AppiumBy.ACCESSIBILITY_ID, "spinInstanceCount"
        )
        assert spin.is_displayed()

    def test_show_virtual_devices_checkbox(self, driver):
        self.navigate_to_device_assignment(driver)
        checkbox = self.wait_for_element(
            driver, AppiumBy.ACCESSIBILITY_ID, "checkShowVirtual"
        )
        assert checkbox.is_displayed()
