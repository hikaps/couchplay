# AGENTS.md - Test Guidelines for CouchPlay

QtTest unit test suite for core manager components (13 test files, ~5.2K lines).

## STRUCTURE

| Test File | Test Class | Lines | Focus |
|-----------|-------------|-------|-------|
| test_couchplayhelper.cpp | CouchPlayHelperTest | 1501 | Helper service (MockSystemOps, 25 overrides) |
| test_devicemanager.cpp | DeviceManagerTest | 664 | Device detection, assignment, stable IDs |
| test_heroicconfigmanager.cpp | HeroicConfigManagerTest | 468 | Heroic config parsing (Legendary/GOG/Nile) |
| test_gamescopeinstance.cpp | GamescopeInstanceTest | 444 | Process wrapping, arg building |
| test_usermanager.cpp | UserManagerTest | 363 | User creation, D-Bus mocking |
| test_sessionmanager.cpp | SessionManagerTest | 330 | Session profiles, layout configs |
| test_sessionrunner.cpp | SessionRunnerTest | 328 | Instance orchestration, mock D-Bus client |
| test_presetmanager.cpp | PresetManagerTest | 245 | Built-in/custom presets, app scanning |
| test_presetmanager_integration.cpp | PresetManagerIntegrationTest | 242 | End-to-end preset + app scanning |
| test_audiomanager.cpp | TestAudioManager | 204 | PipeWire/PulseAudio detection, socket ACL checking |
| test_monitormanager.cpp | MonitorManagerTest | 166 | Display detection |
| test_commandverifier.cpp | CommandVerifierTest | 139 | Path validation, flatpak detection |
| test_scanapplications.cpp | ScanApplicationsTest | 66 | .desktop file scanning |

## WHERE TO LOOK

| Component | Test Coverage |
|-----------|---------------|
| Device hotplug/reconnection | test_devicemanager.cpp: `testGenerateStableId()`, `testRestoreAssignmentsFromStableIds()` |
| Signal emission patterns | All tests: `QSignalSpy` usage |
| File I/O isolation | test_heroicconfigmanager.cpp: `QTemporaryDir`, `QStandardPaths::setTestModeEnabled(true)` |
| D-Bus dependency | test_usermanager.cpp: privileged operations via CouchPlayHelperClient |
| Mock config creation | test_heroicconfigmanager.cpp: `createMockHeroicConfig()`, `createMockGameConfigs()` |

## CONVENTIONS

### Test Lifecycle
- **Class naming**: `Test<Component>` (e.g., `TestDeviceManager`)
- **Slot declarations**: `private Q_SLOTS:`
- **Setup**: `initTestCase()`/`cleanupTestCase()` (once), `init()`/`cleanup()` (per-test)
- **File header**: `QTEST_MAIN(TestClassName)` + `#include "test_<name>.moc"` at EOF

### Assertions
- `QVERIFY(expr)` / `QVERIFY2(expr, msg)` - boolean checks
- `QCOMPARE(actual, expected)` - equality checks
- `QSignalSpy` for async signal verification; `.wait()` for timeouts

### Isolation
- `QTemporaryDir` for filesystem, `QStandardPaths::setTestModeEnabled(true)` for config
- `qgetenv("HOME")` backup/restore for path-dependent tests

### Mocking
- No mocking framework (GMock/QtMock). Build mock data manually via `QTemporaryDir` + `QFile`
- D-Bus tests: Use real helper client, may fail if polkit unavailable

## MOCK PATTERNS
**1. Manual Mock (test_sessionrunner.cpp):**
```cpp
class MockCouchPlayHelperClient : public CouchPlayHelperClient {
    QList<AclCall> aclCalls;  // Record calls for verification
    bool setPathAclWithParents(...) override { aclCalls.append({path, username}); return true; }
};
```

**2. Comprehensive Mock (test_couchplayhelper.cpp):** MockSystemOps overrides 25 SystemOps virtual methods. Key APIs:
- `setFileExists(path, exists)` / `m_files` map for file existence simulation
- `setMockProcessStart(bool)` / `setStandardOutput(data)` for process mocking
- `setAuthResult(bool)` / `setUserExists(user, exists, uid)` for auth/user simulation

**3. Private Member Access (test_sessionrunner.cpp):**
`#define private public` / `#undef private` around includes to test internals.

### Test Method Naming
- `test<Feature>()` for basic, `test<Method><Scenario>()` for method-specific cases
- Group with comments: `// Property tests`, `// Signal tests`

## ANTI-PATTERNS
- **No test doubles framework**: Manual mock creation is verbose
- **Flaky environment tests**: Desktop files or installed flatpaks may not exist
- **D-Bus dependency**: `UserManagerTest` requires helper service running
- **CI exclusions**: 7/13 tests skipped (D-Bus/Polkit/devices) - see `.github/workflows/ci.yml`
- **Partial coverage**: Some managers lack dedicated tests
- **Private member access**: `#define private public` / `#undef private` around includes (test_sessionrunner.cpp, test_audiomanager.cpp)
