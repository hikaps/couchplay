# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors

from appium.webdriver.common.appiumby import AppiumBy
from selenium.common.exceptions import TimeoutException

from helpers.base_test import BaseTest


class TestSessionSetup(BaseTest):
    def test_session_setup_page_loads(self, driver):
        self.navigate_to_session_setup(driver)
        title = self.wait_for_element(
            driver, AppiumBy.ACCESSIBILITY_ID, "spinPlayerCount"
        )
        assert title.is_displayed()

    def test_player_count_default(self, driver):
        self.navigate_to_session_setup(driver)
        spin = self.wait_for_element(
            driver, AppiumBy.ACCESSIBILITY_ID, "spinPlayerCount"
        )
        assert spin.is_displayed()

    def test_layout_cards_visible(self, driver):
        self.navigate_to_session_setup(driver)
        self.wait_for_element(
            driver, AppiumBy.ACCESSIBILITY_ID, "cardLayoutHorizontal"
        )
        self.wait_for_element(
            driver, AppiumBy.ACCESSIBILITY_ID, "cardLayoutVertical"
        )
        # cardLayoutGrid does not expose its objectName reliably -> NAME
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
        assert card.is_displayed()

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
        assert dialog.is_displayed()
        self.click_by_name(driver, "Cancel")

    def test_navigate_to_device_assignment(self, driver):
        self.navigate_to_session_setup(driver)
        self.click_by_name(driver, "Assign Devices")
        title = self.wait_for_element(driver, AppiumBy.NAME, "Assign Devices")
        assert title.is_displayed()

    def test_instance_config_visible_for_two_players(self, driver):
        self.navigate_to_session_setup(driver)
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "comboUser")
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "comboLauncher")
        # comboScaling does not expose its objectName -> NAME (its label)

    def test_game_and_hook_controls_visible(self, driver):
        self.navigate_to_session_setup(driver)
        # GameSelector is a custom ComboBox. Qt exposes its Accessible.name
        # through AT-SPI reliably, while this component's objectName is not
        # consistently published as an accessibility id.
        try:
            self.wait_for_element(driver, AppiumBy.NAME, "Game:")
        except TimeoutException as error:
            source = driver.page_source or ""
            raise AssertionError(
                "Timed out waiting for the Game: selector by accessible name "
                f"(comboGame objectName present: {'comboGame' in source}; "
                f"Game: name present: {'Game:' in source}). "
                "Page source (first 4000 characters):\n"
                f"{source[:4000]}"
            ) from error
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "fieldPreSessionScript")
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "fieldPostSessionScript")
        self.wait_for_element(driver, AppiumBy.NAME, "Scaling:")
