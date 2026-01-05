# AGENTS.md - Agent Guidelines for CouchPlay

## Project Overview

CouchPlay is a KDE/Qt6 Kirigami application for split-screen gaming on Linux. It runs multiple gamescope/Steam instances simultaneously with input device isolation.

- **Language**: C++20, QML
- **Framework**: Qt6, KDE Frameworks 6 (Kirigami)
- **Build System**: CMake with ECM (Extra CMake Modules)
- **License**: GPL-3.0-or-later

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

## Project Structure

```
couchplay/
├── src/
│   ├── core/           # C++ backend logic
│   ├── dbus/           # D-Bus client for helper service
│   └── qml/
│       ├── pages/      # Full-page QML views
│       └── components/ # Reusable QML components
├── helper/             # Privileged D-Bus service (runs as root)
├── tests/              # Qt Test unit tests
├── data/
│   ├── dbus/           # D-Bus service/config files
│   └── polkit/         # PolicyKit action definitions
└── build/              # Build output (not in git)
```

## D-Bus Helper Service

The helper (`couchplay-helper`) runs as a system service with root privileges. It handles:
- Creating Linux users (`CreateUser`)
- Enabling systemd linger (`EnableLinger`)
- Setting Wayland socket ACLs (`SetupWaylandAccess`)
- Changing input device ownership (`ChangeDeviceOwner`)

All privileged operations require PolicyKit authorization.

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

### Gadget Types for QML

Use `Q_GADGET` for value types exposed to QML:

```cpp
struct InputDevice {
    Q_GADGET
    Q_PROPERTY(QString name MEMBER name)
public:
    QString name;
};
Q_DECLARE_METATYPE(InputDevice)
```

### Process Management

Use `QProcess` for spawning external processes. Always connect to signals for async handling:

```cpp
connect(m_process, &QProcess::started, this, &MyClass::onProcessStarted);
connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        this, &MyClass::onProcessFinished);
connect(m_process, &QProcess::errorOccurred, this, &MyClass::onProcessError);
```
