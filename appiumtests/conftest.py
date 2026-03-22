# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors

import os
import subprocess
import sys
import time

import pytest
from appium import webdriver
from appium.options.common.base import AppiumOptions
from appium.webdriver.common.appiumby import AppiumBy
from selenium.webdriver.support.ui import WebDriverWait
from selenium.webdriver.support import expected_conditions as EC

HELPERS_DIR = os.path.join(os.path.dirname(__file__), "helpers")
MOCK_HELPER_SCRIPT = os.path.join(HELPERS_DIR, "mock_helper.py")


def pytest_configure(config):
    config.addinivalue_line(
        "markers", "requires_helper: tests needing D-Bus helper service (skip in CI)"
    )


SCREENSHOT_DIR = os.path.join(os.path.dirname(__file__), "screenshots")
DEFAULT_TIMEOUT = 10


@pytest.fixture(scope="session")
def driver():
    app_id = os.environ.get("COUCHPLAY_APP_ID", "io.github.hikaps.couchplay")

    options = AppiumOptions()
    options.load_capabilities(
        {
            "app": app_id,
            "environ": {
                "QT_LINUX_ACCESSIBILITY_ALWAYS_ON": "1",
            },
            "timeout": 30000,
        }
    )

    driver = webdriver.Remote("http://127.0.0.1:4723", options=options)
    driver.implicitly_wait(0)
    yield driver
    driver.quit()


@pytest.fixture(scope="session")
def mock_helper():
    proc = subprocess.Popen(
        [sys.executable, MOCK_HELPER_SCRIPT],
        stderr=subprocess.PIPE,
    )
    time.sleep(2)
    yield proc
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


@pytest.fixture(scope="session")
def test_users():
    from helpers.test_users import create_test_users, remove_test_users

    create_test_users()
    yield
    remove_test_users()


@pytest.fixture(scope="session")
def virtual_gamepads():
    try:
        from helpers.virtual_devices import (
            create_virtual_gamepads,
            destroy_virtual_gamepads,
        )

        devices = create_virtual_gamepads(2)
        yield devices
        destroy_virtual_gamepads(devices)
    except ImportError:
        pytest.skip("evdev not available — virtual devices disabled")
    except PermissionError:
        pytest.skip("No uinput access — run as root or load uinput module")


@pytest.fixture(autouse=True)
def clean_state(driver):
    go_home(driver)
    yield
    go_home(driver)


def go_home(driver):
    wait = WebDriverWait(driver, DEFAULT_TIMEOUT)
    back_attempts = 0
    max_back = 20
    while back_attempts < max_back:
        try:
            home_page = wait.until(
                EC.presence_of_element_located((AppiumBy.NAME, "Welcome to CouchPlay"))
            )
            if home_page.is_displayed():
                return
        except Exception:
            pass
        driver.back()
        back_attempts += 1
        time.sleep(0.3)


def wait_for_element(driver, by, value, timeout=DEFAULT_TIMEOUT):
    return WebDriverWait(driver, timeout).until(
        EC.presence_of_element_located((by, value))
    )


def wait_for_element_clickable(driver, by, value, timeout=DEFAULT_TIMEOUT):
    return WebDriverWait(driver, timeout).until(EC.element_to_be_clickable((by, value)))


def click_by_name(driver, name, timeout=DEFAULT_TIMEOUT):
    element = wait_for_element_clickable(driver, AppiumBy.NAME, name, timeout)
    element.click()
    return element


def click_by_object_name(driver, object_name, timeout=DEFAULT_TIMEOUT):
    element = wait_for_element_clickable(
        driver, AppiumBy.ACCESSIBILITY_ID, object_name, timeout
    )
    element.click()
    return element


def open_global_drawer(driver):
    driver.open_notifications()
    time.sleep(0.5)
    driver.back()


def navigate_to_page(driver, action_name, page_title):
    open_global_drawer(driver)
    click_by_name(driver, action_name)
    wait_for_element(driver, AppiumBy.NAME, page_title)


@pytest.hookimpl(tryfirst=True, hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    result = outcome.get_result()
    if result.when == "call" and result.failed:
        _driver = item.funcargs.get("driver")
        if _driver:
            os.makedirs(SCREENSHOT_DIR, exist_ok=True)
            filename = f"failed_{item.name}.png"
            filepath = os.path.join(SCREENSHOT_DIR, filename)
            try:
                _driver.save_screenshot(filepath)
            except Exception:
                pass
