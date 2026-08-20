# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors

from appium.webdriver.common.appiumby import AppiumBy
from helpers.base_test import BaseTest


class TestSettings(BaseTest):
    def test_settings_page_loads(self, driver):
        self.navigate_to_settings(driver)
        title = self.wait_for_element(driver, AppiumBy.NAME, "Settings")
        assert title.is_displayed()

    def test_general_section_visible(self, driver):
        self.navigate_to_settings(driver)
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "checkHidePanels")
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "checkKillSteam")
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "checkRestoreSession")

    def test_gamescope_section_visible(self, driver):
        self.navigate_to_settings(driver)
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "comboScaling")
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "comboFilter")
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "checkBorderless")

    def test_reset_action_present(self, driver):
        self.navigate_to_settings(driver)
        self.wait_for_element(driver, AppiumBy.NAME, "Reset to Defaults")

    def test_reset_dialog_opens(self, driver):
        self.navigate_to_settings(driver)
        self.click_by_name(driver, "Reset to Defaults")
        dialog = self.wait_for_element(driver, AppiumBy.NAME, "Reset Settings")
        assert dialog.is_displayed()

    def test_update_section_visible(self, driver):
        self.navigate_to_settings(driver)
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "labelUpdateStatus")
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "btnCheckUpdates")
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "checkUpdateAutomatically")
