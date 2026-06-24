# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors

import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))

import pytest
from appium import webdriver
from appium.options.common.base import AppiumOptions
from appium.webdriver.common.appiumby import AppiumBy
from selenium.webdriver.support.ui import WebDriverWait
from selenium.webdriver.support import expected_conditions as EC

HELPERS_DIR = os.path.join(os.path.dirname(__file__), "helpers")
MOCK_HELPER_SCRIPT = os.path.join(HELPERS_DIR, "mock_helper.py")
STUB_GAMESCOPE = os.path.join(HELPERS_DIR, "stub_gamescope.py")


def pytest_configure(config):
    config.addinivalue_line(
        "markers", "requires_helper: tests needing D-Bus helper service (skip in CI)"
    )


SCREENSHOT_DIR = os.path.join(os.path.dirname(__file__), "screenshots")
DEFAULT_TIMEOUT = 10


@pytest.fixture(scope="session")
def driver(mock_helper):
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
    # Own io.github.hikaps.CouchPlayHelper on the system bus BEFORE the app
    # starts (the client checks availability once at construction). `driver`
    # depends on this fixture, so the mock is up before the app launches. No-op
    # when no system bus is available (smoke tier). Tears down the mock and any
    # child processes (stub gamescope windows) via the process group.
    if not os.path.exists("/run/dbus/system_bus_socket"):
        yield None
        return

    import signal as _signal

    env = {
        **os.environ,
        "QT_QPA_PLATFORM": "wayland",
        "GDK_BACKEND": "wayland",
        "COUCHPLAY_STUB_GAMESCOPE": STUB_GAMESCOPE,
    }
    proc = subprocess.Popen(
        [sys.executable, MOCK_HELPER_SCRIPT],
        stderr=subprocess.PIPE,
        env=env,
        start_new_session=True,
    )
    owned = False
    for _ in range(15):
        r = subprocess.run(
            [
                "dbus-send", "--system", "--dest=org.freedesktop.DBus",
                "--print-reply", "/org/freedesktop/DBus",
                "org.freedesktop.DBus.NameHasOwner",
                "string:io.github.hikaps.CouchPlayHelper",
            ],
            capture_output=True, text=True,
        )
        if "true" in r.stdout:
            owned = True
            break
        time.sleep(1)

    if not owned:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        yield None
        return

    yield proc

    # Kill the whole process group so stub gamescope children are reaped too.
    try:
        os.killpg(os.getpgid(proc.pid), _signal.SIGTERM)
        proc.wait(timeout=5)
    except (subprocess.TimeoutExpired, ProcessLookupError):
        try:
            os.killpg(os.getpgid(proc.pid), _signal.SIGKILL)
        except ProcessLookupError:
            pass


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
    try:
        home_page = wait.until(
            EC.presence_of_element_located((AppiumBy.NAME, "Welcome to CouchPlay"))
        )
        if home_page.is_displayed():
            return
    except Exception:
        pass
    open_global_drawer(driver)
    click_by_name(driver, "Home")
    wait.until(EC.presence_of_element_located((AppiumBy.NAME, "Welcome to CouchPlay")))


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

def click_by_class_name(driver, class_name, timeout=DEFAULT_TIMEOUT):
    element = wait_for_element_clickable(driver, AppiumBy.CLASS_NAME, class_name, timeout)
    element.click()
    return element


def open_global_drawer(driver):
    # Best-effort: on a narrow/mobile layout the global drawer has a handle;
    # on desktop Kirigami the drawer actions live in a persistent sidebar and
    # are already in the AT-SPI tree, so no explicit open is needed. Fail soft.
    wait = WebDriverWait(driver, 2)
    try:
        toggle = wait.until(
            EC.element_to_be_clickable((AppiumBy.ACCESSIBILITY_ID, "drawerToggle"))
        )
        toggle.click()
    except Exception:
        pass
    time.sleep(0.3)


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
