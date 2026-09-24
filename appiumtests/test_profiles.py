# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors

import pytest
import uuid

from selenium.common.exceptions import TimeoutException

from appium.webdriver.common.appiumby import AppiumBy
from helpers.base_test import BaseTest


class TestProfiles(BaseTest):
    def _profile_card(self, driver, profile_name):
        return self.wait_for_element(
            driver, AppiumBy.ACCESSIBILITY_ID, f"profileCard_{profile_name}"
        )

    def _delete_profile_if_present(self, driver, profile_name):
        cards = [
            card
            for card in driver.find_elements(
                AppiumBy.ACCESSIBILITY_ID, f"profileCard_{profile_name}"
            )
            if card.is_displayed()
        ]
        if not cards:
            self.wait_for_absence(
                driver, AppiumBy.ACCESSIBILITY_ID, f"profileCard_{profile_name}"
            )
            return
        delete_button = cards[0].find_element(AppiumBy.ACCESSIBILITY_ID, "btnDeleteProfile")
        delete_button.click()
        self.wait_for_element(driver, AppiumBy.NAME, "Delete Profile")
        self.click_by_name(driver, "Yes")
        self.wait_for_absence(
            driver, AppiumBy.ACCESSIBILITY_ID, f"profileCard_{profile_name}"
        )

    def _finish_profile_test(self, driver, *profile_names):
        dialogs = [
            dialog
            for dialog in driver.find_elements(
                AppiumBy.NAME, "Add Profile to Steam"
            )
            if dialog.is_displayed()
        ]
        if dialogs:
            try:
                self.click_by_name(driver, "Cancel")
            except Exception:
                pass
        self.navigate_to_profiles(driver)
        cleanup_error = None
        for profile_name in profile_names:
            try:
                self._delete_profile_if_present(driver, profile_name)
            except Exception as error:
                cleanup_error = error
        if cleanup_error is not None:
            raise cleanup_error

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
        copy_name = profile_name + " Copy"
        try:
            self.navigate_to_session_setup(driver)
            self.click_by_name(driver, "Save Profile")
            field = self.wait_for_element(
                driver, AppiumBy.ACCESSIBILITY_ID, "fieldProfileName"
            )
            field.send_keys(profile_name)
            self.click_by_name(driver, "Save")

            self.navigate_to_profiles(driver)
            original_card = self._profile_card(driver, profile_name)
            original_card.find_element(
                AppiumBy.ACCESSIBILITY_ID, "btnDuplicateProfile"
            ).click()
            self._profile_card(driver, copy_name)
        finally:
            self._finish_profile_test(driver, copy_name, profile_name)

    def test_add_to_steam_guides_user_when_no_accounts_are_detected(self, driver):
        profile_name = "Steam Test Profile " + uuid.uuid4().hex[:8]
        try:
            self.navigate_to_session_setup(driver)
            self.click_by_name(driver, "Save Profile")
            field = self.wait_for_element(
                driver, AppiumBy.ACCESSIBILITY_ID, "fieldProfileName"
            )
            field.send_keys(profile_name)
            self.click_by_name(driver, "Save")

            self.navigate_to_profiles(driver)
            profile_card = self._profile_card(driver, profile_name)
            profile_card.find_element(
                AppiumBy.ACCESSIBILITY_ID, "btnAddProfileToSteam"
            ).click()
            try:
                dialog = self.wait_for_element(
                    driver, AppiumBy.NAME, "Add Profile to Steam", timeout=30
                )
            except TimeoutException as error:
                source = driver.page_source or ""
                object_name_present = "dialogAddToSteam" in source
                raise AssertionError(
                    "Timed out waiting for the Add Profile to Steam dialog by "
                    f"accessible name (objectName present: {object_name_present}). "
                    "Page source (first 4000 characters):\n"
                    f"{source[:4000]}"
                ) from error
            assert dialog.is_displayed()
            profile_label = self.wait_for_element(
                driver, AppiumBy.ACCESSIBILITY_ID, "labelProfileToAdd"
            )
            assert profile_name in profile_label.text

            game_mode_messages = driver.find_elements(
                AppiumBy.ACCESSIBILITY_ID, "messageSteamGameMode"
            )
            if any(message.is_displayed() for message in game_mode_messages):
                pytest.skip("Steam account discovery is unavailable in Game Mode")

            account_combos = driver.find_elements(AppiumBy.ACCESSIBILITY_ID, "comboSteamAccount")
            visible_combos = [combo for combo in account_combos if combo.is_displayed()]
            if visible_combos:
                pytest.skip(
                    "This host has a Steam account; the empty-account state is not applicable"
                )

            guidance = self.wait_for_element(
                driver, AppiumBy.ACCESSIBILITY_ID, "messageNoSteamAccounts"
            )
            assert guidance.is_displayed()
            assert "Install Steam and sign in" in guidance.text
        finally:
            self._finish_profile_test(driver, profile_name)
