# AGENTS.md - Test Guidelines for CouchPlay

QtTest unit test suite for core manager components.

## STRUCTURE

| Test File | Test Class | Lines | Focus |
|-----------|-------------|-------|-------|
| test_padmap.cpp | PadMapTest | - | Virtual pad axis/button mapping |
| test_devicemanager.cpp | DeviceManagerTest | 664 | Device detection, assignment, stable IDs |
| test_heroicconfigmanager.cpp | HeroicConfigManagerTest | 468 | Heroic config parsing (Legendary/GOG/Nile) |
| test_gamescopeinstance.cpp | GamescopeInstanceTest | 444 | Process wrapping, arg building |
| test_playerstore.cpp | PlayerStoreTest | - | Player name validation, create/delete |
| test_sandboxcommand.cpp | SandboxCommandTest | - | bwrap argv ordering |
| test_sessionmanager.cpp | SessionManagerTest | 330 | Session profiles, layout configs |
| test_sessionrunner.cpp | SessionRunnerTest | 328 | Instance orchestration, fake instances/pad forwarder |
| test_presetmanager.cpp | PresetManagerTest | 245 | Built-in/custom presets, app scanning |
| test_presetmanager_integration.cpp | PresetManagerIntegrationTest | 242 | End-to-end preset + app scanning |
| test_audiomanager.cpp | TestAudioManager | 204 | PipeWire/PulseAudio detection |
| test_monitormanager.cpp | MonitorManagerTest | 166 | Display detection |
| test_commandverifier.cpp | CommandVerifierTest | 139 | Path validation, flatpak detection |
| test_scanapplications.cpp | ScanApplicationsTest | 66 | .desktop file scanning |

## WHERE TO LOOK

| Component | Test Coverage |
|-----------|---------------|
| Device hotplug/reconnection | test_devicemanager.cpp: `testGenerateStableId()`, `testRestoreAssignmentsFromStableIds()` |
| Signal emission patterns | All tests: `QSignalSpy` usage |
| File I/O isolation | test_heroicconfigmanager.cpp: `QTemporaryDir`, `QStandardPaths::setTestModeEnabled(true)` |
| Sandbox/pad seams | test_sessionrunner.cpp: `FakeGamescopeInstance`, `PadForwarder::setProgramOverrideForTesting` |
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
- Host tools (bwrap, uinput) are never required: use the seams below

## MOCK PATTERNS
**1. Fake instance (test_sessionrunner.cpp):**
```cpp
class FakeGamescopeInstance : public GamescopeInstance {
    bool start(const QVariantMap &config, int index, const QList<SandboxMount> &mounts) override; // records config/mounts, emits started
};
runner.setInstanceFactory([](QObject *p) { return new FakeGamescopeInstance(p); });
```

**2. Pad forwarder stub:** `PadForwarder::setProgramOverrideForTesting(script)` points at a temporary shell script that prints `ready /dev/input/event90` and appends `attach` lines to a file.

**3. Pure builders:** `test_sandboxcommand`, `test_padmap`, `test_playerstore` and `test_playerfiles` test argv ordering, axis mapping, name validation and copy semantics without any host dependency.

**4. Private Member Access (test_sessionrunner.cpp):**
`#define private public` / `#undef private` around includes to test internals.

### Test Method Naming
- `test<Feature>()` for basic, `test<Method><Scenario>()` for method-specific cases
- Group with comments: `// Property tests`, `// Signal tests`

## ANTI-PATTERNS
- **No test doubles framework**: Manual mock creation is verbose
- **Flaky environment tests**: Desktop files or installed flatpaks may not exist
- **D-Bus**: tests run under `dbus-run-session` in CI (no real bwrap/uinput/devices needed; the sandbox and `couchplay-padfwd` are faked)
- **CI**: CTest targets run under `dbus-run-session` in `.github/workflows/ci.yml`.
- **Partial coverage**: Some managers lack dedicated tests
- **Private member access**: `#define private public` / `#undef private` around includes (test_sessionrunner.cpp, test_audiomanager.cpp)
