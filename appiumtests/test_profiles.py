# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors

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
