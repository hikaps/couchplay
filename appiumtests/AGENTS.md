# AGENTS.md - E2E Testing Guidelines

## Overview

E2E tests use [selenium-webdriver-at-spi](https://invent.kde.org/sdk/selenium-webdriver-at-spi) to drive the CouchPlay UI via the Linux accessibility bus (AT-SPI2). Tests run on a virtual Wayland session managed by the runner. These tests are **local-only** — they are NOT run in CI (the unit-test tier in `.github/workflows/ci.yml` is the CI gate). Run them manually during development and before release/beta.

## Running Tests

### Locally (requires KDE Plasma Wayland session)

```bash
pip install -r appiumtests/requirements.txt
QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1 selenium-webdriver-at-spi-run pytest appiumtests/ -v
```

### Run only the smoke tier (no helper)

```bash
selenium-webdriver-at-spi-run pytest appiumtests/ -v -m "not requires_helper"
```

### Run a single test file

```bash
selenium-webdriver-at-spi-run pytest appiumtests/test_home.py -v
```

### Run a single test

```bash
selenium-webdriver-at-spi-run pytest appiumtests/test_home.py::TestHomePage::test_app_launches_home_visible -v
```

## Structure

```
appiumtests/
├── conftest.py            # Pytest fixtures, driver lifecycle, failure screenshots
├── helpers/
│   ├── base_test.py       # Shared wait/click/navigation utilities
│   ├── mock_helper.py     # Mock D-Bus helper service (29 methods)
│   ├── test_users.py      # Linux user creation/cleanup for tests
│   └── virtual_devices.py # Virtual gamepad creation via uinput
├── test_home.py           # HomePage smoke tests (P0)
├── test_session_setup.py  # SessionSetupPage tests (P1)
├── test_session.py        # Session lifecycle with mock helper
├── test_profiles.py       # Profile management (P2)
├── test_settings.py       # Settings tests (P2)
├── test_devices.py        # Device assignment (requires mock helper)
├── test_users.py          # User management (requires mock helper)
└── requirements.txt
```

## Conventions

### Test Organization

- Class per page: `Test<PageName>`
- Method naming: `test_<action>_<expected_result>()`
- Priority tiers: P0 (smoke), P1 (core flows), P2 (secondary pages), P3 (helper-dependent)

### Session Testing

Session tests (`test_session.py`) use a **mock D-Bus helper** that runs on the system bus. The mock implements the current `io.github.hikaps.CouchPlayHelper` interface exactly (see `helper/CouchPlayHelper.h`) — `LaunchInstance(username, compositorUid, gamescopeArgs, gameCommand, environment, bindPaths)` returns a fake PID without spawning gamescope. This enables full session lifecycle testing without real hardware.

Each `LaunchInstance()` call is recorded (JSON-per-line) to `$COUCHPLAY_MOCK_LAUNCH_LOG` (default `/tmp/couchplay-mock-launch.jsonl`) so multi-instance assertions can verify N distinct launches with the correct per-user command/args/disjoint geometry.

**Session fixtures** (session-scoped, in conftest.py):
- `mock_helper` — starts the mock D-Bus helper Python process
- `test_users` — creates `player2` and `player3` Linux users
- `virtual_gamepads` — creates 2 virtual gamepads via uinput (requires root or uinput module)

Tests that use these fixtures are automatically opted into session testing.

### Element Selection

Priority order:
1. `AppiumBy.ACCESSIBILITY_ID` → maps to `objectName` (most reliable)
2. `AppiumBy.NAME` → maps to `Accessible.name` (localized text)
3. `AppiumBy.CLASS_NAME` → last resort (`[role | name]` format)

### Markers

- `@pytest.mark.requires_helper` — tests needing the mock D-Bus helper on the system bus (so the GUI believes a helper is available). Omit the marker for pure UI smoke tests.

### Timing

- Default timeout: 10 seconds (conftest.py `DEFAULT_TIMEOUT`)
- Always use `WebDriverWait` — never `time.sleep()` except in `go_home()` for page transition delays

### Test Isolation

- `clean_state` fixture (autouse) navigates to home page before and after each test
- Tests must not depend on state from other tests

## Adding New Tests

1. Add `objectName` and `Accessible.*` to the QML element (see root AGENTS.md naming conventions)
2. Create or extend the test file for the target page
3. Inherit from `BaseTest` for shared utilities
4. Use `self.wait_for_element()` / `self.click_by_object_name()` instead of raw driver calls
5. If the test needs the D-Bus helper, add `pytestmark = pytest.mark.requires_helper` at module level

## When to run

E2E is intentionally **not** in CI — it needs a real Wayland session, AT-SPI, and (for session tests) the system-bus mock helper + pre-created users, which is environment-fragile and expensive. Run it:

- **During development**: `selenium-webdriver-at-spi-run pytest appiumtests/ -v` against your live KDE Plasma Wayland session (fast TDD loop for UI wiring).
- **Before release/beta**: the full suite, including the `requires_helper` session tests, with `player2`/`player3` users and the mock helper on the system bus. For full-fidelity two-user-session + audio-bifurcation verification, use an isolated libvirt VM with real PipeWire + the real helper + a stub gamescope (see root `AGENTS.md` test-tier section).

Failure screenshots are written to `appiumtests/screenshots/`.
