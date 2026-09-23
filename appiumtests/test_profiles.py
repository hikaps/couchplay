# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors

import pytest
import uuid

from appium.webdriver.common.appiumby import AppiumBy
from helpers.base_test import BaseTest


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

    def test_add_to_steam_guides_user_when_no_accounts_are_detected(self, driver):
        profile_name = "Steam Test Profile " + uuid.uuid4().hex[:8]
        self.navigate_to_session_setup(driver)
        self.click_by_name(driver, "Save Profile")
        field = self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "fieldProfileName")
        field.send_keys(profile_name)
        self.click_by_name(driver, "Save")

        self.navigate_to_profiles(driver)
        self.wait_for_element(driver, AppiumBy.NAME, profile_name)
        self.click_by_object_name(driver, "btnAddProfileToSteam")
        dialog = self.wait_for_element(
            driver, AppiumBy.ACCESSIBILITY_ID, "dialogAddToSteam", timeout=30
        )
        assert dialog.is_displayed()

        game_mode_messages = driver.find_elements(
            AppiumBy.ACCESSIBILITY_ID, "messageSteamGameMode"
        )
        if any(message.is_displayed() for message in game_mode_messages):
            pytest.skip("Steam account discovery is unavailable in Game Mode")

        account_combos = driver.find_elements(AppiumBy.ACCESSIBILITY_ID, "comboSteamAccount")
        visible_combos = [combo for combo in account_combos if combo.is_displayed()]
        if visible_combos:
            account = visible_combos[0]
            value = (account.get_attribute("value") or "").strip()
            visible_text = account.text.strip()
            if (value and value != "Steam account") or (
                visible_text and visible_text != "Steam account"
            ):
                pytest.skip(
                    "This host has a Steam account; the empty-account state is not applicable"
                )

        guidance = self.wait_for_element(
            driver, AppiumBy.ACCESSIBILITY_ID, "messageNoSteamAccounts"
        )
        assert guidance.is_displayed()
        assert "Install Steam and sign in" in guidance.text
        self.wait_for_absence(driver, AppiumBy.ACCESSIBILITY_ID, "comboSteamAccount")

