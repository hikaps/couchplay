# AGENTS.md - Agent Guidelines for CouchPlay

CouchPlay is a C++20/QML KDE/Qt6 Kirigami application for split-screen gaming on Linux (GPL-3.0-or-later).

## Build Environment

This project is developed on **Bazzite** (immutable Fedora with Wayland/KDE). Building must be done inside a distrobox container, but the application runs on the host.

### Build Commands

```bash
# Configure (first time only)
distrobox enter fedora-dev -- bash -c "cd /var/home/notaname/Repos/splitscreen && cmake -B build"

# Build
distrobox enter fedora-dev -- bash -c "cd /var/home/notaname/Repos/splitscreen && cmake --build build"

# Run (on HOST - gamescope requires host environment)
./build/bin/couchplay

# Clean rebuild
distrobox enter fedora-dev -- bash -c "cd /var/home/notaname/Repos/splitscreen && rm -rf build && cmake -B build && cmake --build build"
```

### Test Commands

```bash
# Run all tests
distrobox enter fedora-dev -- bash -c "cd /var/home/notaname/Repos/splitscreen/build && ctest --output-on-failure"

# Run a single test by name
distrobox enter fedora-dev -- bash -c "cd /var/home/notaname/Repos/splitscreen/build && ctest -R DeviceManagerTest --output-on-failure"
distrobox enter fedora-dev -- bash -c "cd /var/home/notaname/Repos/splitscreen/build && ctest -R GamescopeInstanceTest --output-on-failure"

# Run single test executable directly (more verbose output)
distrobox enter fedora-dev -- bash -c "cd /var/home/notaname/Repos/splitscreen/build && ./bin/test_gamescopeinstance"

# List available tests
distrobox enter fedora-dev -- bash -c "cd /var/home/notaname/Repos/splitscreen/build && ctest -N"
```

Test names follow the pattern: `test_<component>.cpp` -> `<Component>Test` (e.g., `test_devicemanager.cpp` -> `DeviceManagerTest`).

## Code Style Guidelines

### File Headers

All source files must include SPDX license headers:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors
```

### Include Order

1. Own header (for .cpp files)
2. Qt headers (alphabetical)
3. KDE Frameworks headers
4. System headers
5. Project headers

```cpp
#include "GamescopeInstance.h"

#include <QDBusConnection>
#include <QDebug>
#include <QProcess>

#include <KConfig>

#include <pwd.h>
#include <unistd.h>

#include "SessionManager.h"
```

### Naming Conventions

| Element | Convention | Example |
|---------|------------|---------|
| Classes | PascalCase | `DeviceManager`, `GamescopeInstance` |
| Member variables | m_camelCase | `m_devices`, `m_isPrimary` |
| Methods | camelCase | `buildGamescopeArgs()`, `setStatus()` |
| Qt signals | camelCase, past tense | `devicesChanged()`, `errorOccurred()` |
| Qt slots | on + Source + Event | `onProcessStarted()`, `onReadyReadStandardError()` |
| Constants | SCREAMING_SNAKE_CASE | `ACTION_MANAGE_DEVICES` |
| Q_PROPERTY | camelCase | `instanceCount`, `currentLayout` |
| QML files | PascalCase | `HomePage.qml`, `DeviceCard.qml` |

### Qt/KDE Conventions

- Use `QStringLiteral()` for all string literals (avoids runtime allocation)
- Use `Q_EMIT` instead of `emit` for signal emission
- Use `Q_SIGNALS` and `Q_SLOTS` instead of `signals` and `slots`
- Use `#pragma once` instead of include guards
- Use `nullptr` instead of `NULL` or `0`
- Use `override` for virtual method overrides
- Use `= default` and `= delete` appropriately

```cpp
// Good
Q_EMIT errorOccurred(QStringLiteral("Failed to start"));
m_status = QStringLiteral("Running");

// Bad
emit errorOccurred("Failed to start");
m_status = "Running";
```

### QML/Kirigami Conventions

- Import with aliases: `import org.kde.kirigami as Kirigami`
- Use `i18nc()` for all user-visible strings with context
- Component IDs use camelCase: `id: deviceManager`
- Properties use `required property` for mandatory injections

```qml
import QtQuick
import org.kde.kirigami as Kirigami

Kirigami.Page {
    title: i18nc("@title", "Device Assignment")
    
    required property DeviceManager deviceManager
}
```

### Class Structure

Follow this order in class declarations:

1. Q_OBJECT macro
2. QML_ELEMENT (if exposed to QML)
3. Q_PROPERTY declarations
4. public: constructors/destructor
5. public: Q_INVOKABLE methods
6. public: regular methods and property getters/setters
7. Q_SIGNALS:
8. public/private Q_SLOTS:
9. private: helper methods
10. private: member variables

### Error Handling

- Use `qWarning()` for recoverable errors and warnings
- Use `qDebug()` for development/debugging output (avoid in production paths)
- Emit `errorOccurred(QString)` signal for user-facing errors
- Check return values and handle failures gracefully
- Clean up resources in destructors

```cpp
if (!m_process->waitForStarted(3000)) {
    qWarning() << "Instance" << m_index << "failed to start:" << m_process->errorString();
    Q_EMIT errorOccurred(QStringLiteral("Failed to start gamescope"));
    return false;
}
```

### Testing Conventions

- Test classes inherit from `QObject` with `Q_OBJECT` macro
- Use `private Q_SLOTS` for test methods
- Test method names: `test<Feature>` or `test<MethodName><Scenario>`
- Use `QVERIFY()`, `QCOMPARE()`, `QVERIFY2()` for assertions
- Use `QSignalSpy` for signal verification
- End test files with `QTEST_MAIN(TestClassName)` and `#include "filename.moc"`

```cpp
void TestDeviceManager::testParseDevices()
{
    DeviceManager manager;
    manager.refresh();
    QVERIFY(!manager.devices().isEmpty());
}

QTEST_MAIN(TestDeviceManager)
#include "test_devicemanager.moc"
```

## Common Patterns

### Exposing C++ to QML

```cpp
// In header
class MyManager : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    
public:
    Q_INVOKABLE void doSomething();
};
```

## Branch Documentation (Local Reference Only)

**Important:** The `docs/branches/` directory is kept locally for development reference only and is not committed to git (see `.gitignore`).

When working on feature branches, you may maintain local reference files:

- **`PLAN.md`** - Implementation plan, design decisions, task breakdown, and requirements
- **`PROGRESS.md`** - Development progress, completed tasks, blockers, and session logs

### Local Development Guidelines

1. **Before starting work on a branch**, create or review local reference files to understand:
   - Design decisions already made
   - Tasks completed and remaining
   - Any blockers or dependencies

2. **Document significant decisions** in your local PLAN.md for future reference:
   - What was decided and why
   - Any alternatives considered

3. **Update your local PROGRESS.md** after completing tasks as a personal development log:
   - Mark tasks as complete
   - Note any issues encountered
   - Add session log entries with date

4. **When encountering blockers**, document locally for team discussion:
   - What is blocked
   - Why (dependency, technical issue, etc.)
   - What needs to happen to unblock

### Branch Naming Convention

Feature branches follow the pattern: `feature/<feature-name>`
