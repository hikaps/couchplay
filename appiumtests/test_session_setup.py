# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors

from appium.webdriver.common.appiumby import AppiumBy
from helpers.base_test import BaseTest


class TestSessionSetup(BaseTest):
    def test_session_setup_page_loads(self, driver):
        self.navigate_to_session_setup(driver)
        title = self.wait_for_element(
            driver, AppiumBy.ACCESSIBILITY_ID, "spinPlayerCount"
        )

    def test_player_count_default(self, driver):
        self.navigate_to_session_setup(driver)
        spin = self.wait_for_element(
            driver, AppiumBy.ACCESSIBILITY_ID, "spinPlayerCount"
        )

    def test_layout_cards_visible(self, driver):
        self.navigate_to_session_setup(driver)
        self.wait_for_element(
            driver, AppiumBy.ACCESSIBILITY_ID, "cardLayoutHorizontal"
        )
        self.wait_for_element(
            driver, AppiumBy.ACCESSIBILITY_ID, "cardLayoutVertical"
        )
        # cardLayoutGrid objectName does not propagate to AT-SPI in this Qt
        # build (the Horizontal/Vertical/MultiMonitor siblings do); use NAME.
        self.wait_for_element(driver, AppiumBy.NAME, "Grid")
        self.wait_for_element(
            driver, AppiumBy.ACCESSIBILITY_ID, "cardLayoutMultiMonitor"
        )

    def test_select_layout(self, driver):
        self.navigate_to_session_setup(driver)
        self.click_by_object_name(driver, "cardLayoutVertical")
        card = self.wait_for_element(
            driver, AppiumBy.ACCESSIBILITY_ID, "cardLayoutVertical"
        )

    def test_toolbar_actions_present(self, driver):
        self.navigate_to_session_setup(driver)
        # Toolbar actions are Kirigami.Action -> no objectName, use NAME
        self.wait_for_element(driver, AppiumBy.NAME, "Start Session")
        self.wait_for_element(driver, AppiumBy.NAME, "Assign Devices")
        self.wait_for_element(driver, AppiumBy.NAME, "Save Profile")

    def test_save_profile_dialog_opens(self, driver):
        self.navigate_to_session_setup(driver)
        self.click_by_name(driver, "Save Profile")
        # Dialog title "Save Profile" collides with the action; assert subtitle
        dialog = self.wait_for_element(
            driver, AppiumBy.NAME, "Enter a name for this session profile"
        )

    def test_navigate_to_device_assignment(self, driver):
        self.navigate_to_session_setup(driver)
        self.click_by_name(driver, "Assign Devices")
        title = self.wait_for_element(driver, AppiumBy.NAME, "Assign Devices")

    def test_instance_config_visible_for_two_players(self, driver):
        self.navigate_to_session_setup(driver)
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "comboUser")
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "comboLauncher")
        # comboScaling objectName does not propagate from the instance-card
        # FormLayout here (it does on the Settings page; siblings comboUser /
        # comboLauncher propagate); use NAME (the FormData label).
        self.wait_for_element(driver, AppiumBy.NAME, "Scaling:")
