# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors

from appium.webdriver.common.appiumby import AppiumBy
from helpers.base_test import BaseTest


class TestHomePage(BaseTest):
    def test_app_launches_home_visible(self, driver):
        heading = self.wait_for_element(driver, AppiumBy.NAME, "Welcome to CouchPlay")
        assert heading.is_displayed()

    def test_home_shows_action_cards(self, driver):
        self.click_by_object_name(driver, "cardNewSession")
        self.wait_for_element(driver, AppiumBy.NAME, "New Session")

    def test_navigate_to_session_setup_via_card(self, driver):
        self.click_by_object_name(driver, "cardNewSession")
        title = self.wait_for_element(driver, AppiumBy.NAME, "New Session")
        assert title.is_displayed()

    def test_navigate_to_profiles_via_card(self, driver):
        self.click_by_object_name(driver, "cardLoadProfile")
        title = self.wait_for_element(driver, AppiumBy.NAME, "Profiles")
        assert title.is_displayed()

    def test_navigate_to_profiles_via_drawer(self, driver):
        self.navigate_to_profiles(driver)
        title = self.wait_for_element(driver, AppiumBy.NAME, "Profiles")
        assert title.is_displayed()

    def test_navigate_to_users_via_drawer(self, driver):
        self.navigate_to_users(driver)
        title = self.wait_for_element(driver, AppiumBy.NAME, "Users")
        assert title.is_displayed()

    def test_navigate_to_settings_via_drawer(self, driver):
        self.navigate_to_settings(driver)
        title = self.wait_for_element(driver, AppiumBy.NAME, "Settings")
        assert title.is_displayed()
