# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors

import pytest
from appium.webdriver.common.appiumby import AppiumBy
from helpers.base_test import BaseTest

pytestmark = pytest.mark.requires_helper


class TestUsers(BaseTest):
    def test_users_page_loads(self, driver):
        self.navigate_to_users(driver)
        title = self.wait_for_element(driver, AppiumBy.NAME, "Users")
        assert title.is_displayed()

    def test_toolbar_actions_present(self, driver):
        self.navigate_to_users(driver)
        self.wait_for_element(driver, AppiumBy.NAME, "Add User")
        self.wait_for_element(driver, AppiumBy.NAME, "Refresh")

    def test_add_user_dialog_opens(self, driver):
        self.navigate_to_users(driver)
        self.click_by_name(driver, "Add User")
        dialog = self.wait_for_element(
            driver, AppiumBy.ACCESSIBILITY_ID, "dialogAddUser"
        )
        assert dialog.is_displayed()

    def test_add_user_dialog_has_fields(self, driver):
        self.navigate_to_users(driver)
        self.click_by_name(driver, "Add User")
        self.wait_for_element(driver, AppiumBy.ACCESSIBILITY_ID, "fieldUsername")

    def test_helper_status_visible(self, driver):
        self.navigate_to_users(driver)
        self.wait_for_element(driver, AppiumBy.NAME, "CouchPlay Users")
