# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors

from appium.webdriver.common.appiumby import AppiumBy
from helpers.base_test import BaseTest


class TestSessionSetup(BaseTest):
    def test_session_setup_page_loads(self, driver):
        self.navigate_to_session_setup(driver)
        title = self.wait_for_element(driver, AppiumBy.NAME, "New Session")
        assert title.is_displayed()

    def test_player_count_default(self, driver):
        self.navigate_to_session_setup(driver)
        spin = self.wait_for_element(
            driver, AppiumBy.ACCESSIBILITY_ID, "spinPlayerCount"
        )
        assert spin.is_displayed()

    def test_layout_cards_visible(self, driver):
        self.navigate_to_session_setup(driver)
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "cardLayoutHorizontal")
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "cardLayoutVertical")
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "cardLayoutGrid")
        self.wait_for_element(
            driver, AppiumBy.ACCESSIBILITY_ID, "cardLayoutMultiMonitor"
        )

    def test_select_layout(self, driver):
        self.navigate_to_session_setup(driver)
        self.click_by_object_name(driver, "cardLayoutVertical")
        card = self.wait_for_element(
            driver, AppiumBy.ACCESSIBILITY_ID, "cardLayoutVertical"
        )
        assert card.is_displayed()

    def test_toolbar_actions_present(self, driver):
        self.navigate_to_session_setup(driver)
        self.wait_for_element(driver, AppiumBy.NAME, "Start Session")
        self.wait_for_element(driver, AppiumBy.NAME, "Assign Devices")
        self.wait_for_element(driver, AppiumBy.NAME, "Save Profile")

    def test_save_profile_dialog_opens(self, driver):
        self.navigate_to_session_setup(driver)
        self.click_by_name(driver, "Save Profile")
        dialog = self.wait_for_element(
            driver, AppiumBy.ACCESSIBILITY_ID, "dialogSaveProfile"
        )
        assert dialog.is_displayed()

    def test_navigate_to_device_assignment(self, driver):
        self.navigate_to_session_setup(driver)
        self.click_by_name(driver, "Assign Devices")
        title = self.wait_for_element(driver, AppiumBy.NAME, "Assign Devices")
        assert title.is_displayed()

    def test_instance_config_visible_for_two_players(self, driver):
        self.navigate_to_session_setup(driver)
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "comboUser")
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "comboLauncher")
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "comboScaling")
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "checkBorderless")
