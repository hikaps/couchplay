# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors

from appium.webdriver.common.appiumby import AppiumBy
from helpers.base_test import BaseTest
import uuid


class TestProfiles(BaseTest):
    def test_profiles_page_loads(self, driver):
        self.navigate_to_profiles(driver)
        title = self.wait_for_element(driver, AppiumBy.NAME, "Profiles")
        assert title.is_displayed()

    def test_empty_state_visible(self, driver):
        self.navigate_to_profiles(driver)
        empty_msg = self.wait_for_element(driver, AppiumBy.NAME, "No Saved Profiles")
        assert empty_msg.is_displayed()

    def test_toolbar_actions_present(self, driver):
        self.navigate_to_profiles(driver)
        self.wait_for_element(driver, AppiumBy.NAME, "New Profile")
        self.wait_for_element(driver, AppiumBy.NAME, "Refresh")

    def test_new_profile_navigates_to_session_setup(self, driver):
        self.navigate_to_profiles(driver)
        self.click_by_name(driver, "New Profile")
        title = self.wait_for_element(driver, AppiumBy.NAME, "New Session")
        assert title.is_displayed()

    def test_duplicate_profile_creates_copy(self, driver):
        profile_name = "Automation Profile " + uuid.uuid4().hex[:8]
        self.navigate_to_session_setup(driver)
        self.click_by_name(driver, "Save Profile")
        field = self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "fieldProfileName")
        field.send_keys(profile_name)
        self.click_by_name(driver, "Save")

        self.navigate_to_profiles(driver)
        self.wait_for_element(driver, AppiumBy.NAME, profile_name)
        self.click_by_object_name(driver, "btnDuplicateProfile")
        self.wait_for_element(driver, AppiumBy.NAME, profile_name + " Copy")

    def test_add_to_steam_shows_manual_steam_warning(self, driver):
        profile_name = "Steam Test Profile " + uuid.uuid4().hex[:8]
        self.navigate_to_session_setup(driver)
        self.click_by_name(driver, "Save Profile")
        field = self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "fieldProfileName")
        field.send_keys(profile_name)
        self.click_by_name(driver, "Save")

        try:
            self.navigate_to_profiles(driver)
            profile_card = self.wait_for_element(
                driver, AppiumBy.ACCESSIBILITY_ID, f"profileCard_{profile_name}"
            )
            profile_card.find_element(
                AppiumBy.ACCESSIBILITY_ID, "btnAddProfileToSteam"
            ).click()
            dialog = self.wait_for_element(
                driver, AppiumBy.NAME, "Add Profile to Steam"
            )
            assert dialog.is_displayed()
            warning = self.wait_for_element(
                driver, AppiumBy.ACCESSIBILITY_ID, "messageCloseSteam"
            )
            assert warning.is_displayed()
            assert "CLOSE STEAM MANUALLY BEFORE ADDING" in warning.text
            assert "Reopen Steam manually afterward" in warning.text
            account_selector = self.wait_for_element(
                driver, AppiumBy.ACCESSIBILITY_ID, "comboSteamAccount"
            )
            assert account_selector.is_displayed()
        finally:
            dialogs = [
                dialog
                for dialog in driver.find_elements(
                    AppiumBy.NAME, "Add Profile to Steam"
                )
                if dialog.is_displayed()
            ]
            if dialogs:
                self.click_by_name(driver, "Cancel")
            self.navigate_to_profiles(driver)
            cards = driver.find_elements(
                AppiumBy.ACCESSIBILITY_ID, f"profileCard_{profile_name}"
            )
            if cards:
                cards[0].find_element(
                    AppiumBy.ACCESSIBILITY_ID, "btnDeleteProfile"
                ).click()
                self.click_by_name(driver, "Yes")
