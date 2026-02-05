# AGENTS.md - Test Guidelines for CouchPlay

QtTest unit test suite for core manager components (11 test files, ~7.2K lines).

## STRUCTURE

| Test File | Test Class | Lines | Focus |
|-----------|-------------|-------|-------|
| test_devicemanager.cpp | DeviceManagerTest | 671 | Device detection, assignment, stable IDs |
| test_gamelibrary.cpp | GameLibraryTest | 606 | Game CRUD, persistence, shortcuts |
| test_gamescopeinstance.cpp | GamescopeInstanceTest | 446 | Process wrapping, arg building |
| test_presetmanager.cpp | PresetManagerTest | 479 | Built-in/custom presets, app scanning |
| test_sessionmanager.cpp | SessionManagerTest | 364 | Session profiles, layout configs |
| test_usermanager.cpp | UserManagerTest | 363 | User creation, D-Bus mocking |
| test_heroicconfigmanager.cpp | HeroicConfigManagerTest | 340 | Heroic config parsing (Legendary/GOG/Nile) |
| test_monitormanager.cpp | MonitorManagerTest | 166 | Display detection |
| test_commandverifier.cpp | CommandVerifierTest | 102 | Path validation, flatpak detection |
| test_scanapplications.cpp | ScanApplicationsTest | 65 | .desktop file scanning |

## WHERE TO LOOK

| Component | Test Coverage |
|-----------|---------------|
| Device hotplug/reconnection | test_devicemanager.cpp: `testGenerateStableId()`, `testRestoreAssignmentsFromStableIds()` |
| Signal emission patterns | All tests: `QSignalSpy` usage for async verification |
| File I/O isolation | test_gamelibrary.cpp, test_heroicconfigmanager.cpp: `QTemporaryDir`, `QStandardPaths::setTestModeEnabled(true)` |
| D-Bus dependency | test_usermanager.cpp: Includes CouchPlayHelperClient, tests privileged operations |
| Mock config creation | test_heroicconfigmanager.cpp: `createMockHeroicConfig()`, `createMockGameConfigs()` |

## CONVENTIONS

### Test Lifecycle
- **Class naming**: `Test<Component>` (e.g., `TestDeviceManager`)
- **Slot declarations**: `private Q_SLOTS:`
- **Setup/teardown**: 
  - `initTestCase()` / `cleanupTestCase()` - once per test class
  - `init()` / `cleanup()` - before/after each test (optional)
- **File header**: `QTEST_MAIN(TestClassName)` + `#include "test_<name>.moc"` at EOF

### Assertions
- `QVERIFY(expr)` - boolean checks
- `QCOMPARE(actual, expected)` - equality checks
- `QVERIFY2(expr, message)` - boolean with failure message
- `QSignalSpy(spy, signal)` - async signal verification; use `.wait()` for timeouts

### Isolation Patterns
- **Temp directories**: `QTemporaryDir` for filesystem isolation
- **Test mode**: `QStandardPaths::setTestModeEnabled(true)` for config file isolation
- **Environment**: `qgetenv("HOME")` backup/restore for path-dependent tests
- **Helper macros**: `#define KEY(x) QStringLiteral(x)` for QVariantMap keys (repetitive)

### Mocking
- Manual mock file creation via `QTemporaryDir` + `QFile` + `QJsonDocument`
- No mocking framework (GMock/QtMock) - build mock data manually
- D-Bus tests: Use real helper client, may fail if polkit unavailable

### Test Method Naming
- `test<Feature>()` - basic functionality (e.g., `testInitialization()`)
- `test<Method><Scenario>()` - method-specific cases (e.g., `testAddGameDuplicate()`)
- Group related tests with comments (e.g., `// Property tests`, `// Signal tests`)

## ANTI-PATTERNS

- **No test doubles framework**: Mocking requires manual file creation - verbose
- **Flaky environment tests**: Tests assuming specific desktop files or installed flatpaks may fail
- **D-Bus dependency**: `UserManagerTest` requires helper service running
- **No CI/CD**: Tests run locally only; no automated pipeline to catch regressions
- **Verbose mocking**: Creating mock config files inline in tests (see `test_heroicconfigmanager.cpp`)
- **Partial coverage**: Some managers (AudioManager, SessionRunner) lack dedicated tests
