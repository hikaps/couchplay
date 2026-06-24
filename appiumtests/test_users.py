# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors

from appium.webdriver.common.appiumby import AppiumBy
from helpers.base_test import BaseTest


class TestUsers(BaseTest):
    def test_users_page_loads(self, driver):
        self.navigate_to_users(driver)
        title = self.wait_for_element(driver, AppiumBy.NAME, "Users")
        assert title.is_displayed()

    def test_toolbar_actions_present(self, driver):
        self.navigate_to_users(driver)
        # Toolbar actions are Kirigami.Action -> no objectName, use NAME
        self.wait_for_element(driver, AppiumBy.NAME, "Add User")
        self.wait_for_element(driver, AppiumBy.NAME, "Refresh")

    def test_add_user_dialog_opens(self, driver):
        self.navigate_to_users(driver)
        self.click_by_name(driver, "Add User")
        # Dialog (and its field) don't expose objectName; the confirm button is
        # only present while the dialog is open.
        dialog = self.wait_for_element(driver, AppiumBy.NAME, "Create User")
        assert dialog.is_displayed()

    def test_add_user_dialog_has_fields(self, driver):
        self.navigate_to_users(driver)
        self.click_by_name(driver, "Add User")
        self.wait_for_element(driver, AppiumBy.NAME, "Username")

    def test_helper_status_visible(self, driver):
        self.navigate_to_users(driver)
        self.wait_for_element(driver, AppiumBy.NAME, "CouchPlay Users")
