# CouchPlay - Project Plan

A KDE Plasma application for running multiple Steam/gamescope instances simultaneously with split-screen gaming, input device assignment, and session management.

## Overview

CouchPlay enables local multiplayer gaming on Linux by:
- Running multiple gamescope instances (starting with 2)
- Assigning input devices (controllers/keyboards) to specific instances
- Managing screen layouts (split-screen, multi-monitor)
- Saving session profiles for quick launch
- Creating desktop shortcuts for games

## Technology Stack

| Component | Technology |
|-----------|------------|
| **GUI Framework** | Qt6/QML + Kirigami (KDE Frameworks 6) |
| **Backend Logic** | C++ with Qt |
| **Privileged Helper** | C++ D-Bus service with Polkit |
| **Build System** | CMake + ECM (Extra CMake Modules) |
| **Packaging** | Flatpak |
| **Configuration Storage** | KConfig |

## App Identity

- **App ID**: `io.github.hikaps.couchplay`
- **Name**: CouchPlay
- **License**: GPL-3.0 (standard for KDE apps)

## Project Structure

```
couchplay/
├── CMakeLists.txt
├── io.github.hikaps.couchplay.desktop
├── io.github.hikaps.couchplay.metainfo.xml
├── io.github.hikaps.couchplay.json              # Flatpak manifest
├── PLAN.md                                       # This file
├── README.md
├── data/
│   ├── icons/
│   │   └── couchplay.svg
│   ├── polkit/
│   │   └── io.github.hikaps.couchplay.policy
│   └── dbus/
│       ├── io.github.hikaps.CouchPlayHelper.service
│       └── io.github.hikaps.CouchPlayHelper.conf
├── scripts/
│   └── install-helper.sh                         # Install script for privileged helper
├── src/
│   ├── CMakeLists.txt
│   ├── main.cpp
│   ├── core/
│   │   ├── SessionManager.cpp/.h                 # Session profiles management
│   │   ├── DeviceManager.cpp/.h                  # Input device detection/assignment
│   │   ├── GamescopeInstance.cpp/.h              # Gamescope process wrapper
│   │   ├── UserManager.cpp/.h                    # Linux user management
│   │   ├── MonitorManager.cpp/.h                 # Display detection
│   │   ├── GameLibrary.cpp/.h                    # Game detection and shortcuts
│   │   └── AudioManager.cpp/.h                   # PipeWire configuration
│   ├── dbus/
│   │   └── CouchPlayHelperClient.cpp/.h          # D-Bus client for privileged ops
│   └── qml/
│       ├── Main.qml
│       ├── pages/
│       │   ├── HomePage.qml                      # Dashboard/quick start
│       │   ├── SessionSetupPage.qml              # Configure new session
│       │   ├── DeviceAssignmentPage.qml          # Assign controllers/keyboards
│       │   ├── LayoutPage.qml                    # Screen layout configuration
│       │   ├── ProfilesPage.qml                  # Saved session profiles
│       │   ├── GamesPage.qml                     # Game library and shortcuts
│       │   ├── UsersPage.qml                     # User management
│       │   └── SettingsPage.qml                  # Global settings
│       └── components/
│           ├── DeviceCard.qml                    # Input device UI card
│           ├── InstanceCard.qml                  # Gamescope instance card
│           ├── LayoutPreview.qml                 # Visual layout preview
│           ├── MonitorSelector.qml               # Monitor selection widget
│           └── GameCard.qml                      # Game shortcut card
├── helper/
│   ├── CMakeLists.txt
│   ├── main.cpp
│   └── CouchPlayHelper.cpp/.h                    # Privileged D-Bus service
└── tests/
    └── ...
```

## Core Features

### 1. Session Management
- Create/edit/delete session profiles
- Save configurations with custom names
- Quick launch from saved profiles
- Auto-restore last session option

### 2. Gamescope Instance Configuration
Per-instance settings:
- Internal (game) and output (window) resolution
- Refresh rate / FPS limit
- Scaling mode (FSR, NIS, integer, fit, stretch)
- Monitor and screen region
- Linux user account to run as

### 3. Input Device Assignment
- Detect devices from `/proc/bus/input/devices`
- Identify controllers, keyboards, mice
- Visual drag-and-drop assignment
- Polkit helper for device ownership transfer
- Automatic cleanup on exit

### 4. Screen Layout Configuration
- Horizontal split (side-by-side)
- Vertical split (top/bottom)
- Multi-monitor (each instance on different monitor)
- Custom ratios (future: 2x2 grid, custom regions)

### 5. User Management
- List existing Linux users
- Create new users for gaming
- Configure per-user settings
- Audio routing (PipeWire TCP auto-configuration)

### 6. Game Library
- Add games manually (Steam App ID or executable path)
- Create desktop shortcuts for quick launch
- Per-game session profiles
- Launch specific games in specific instances

## Privileged Helper Design

### Polkit Policy
Actions requiring authentication:
- `io.github.hikaps.couchplay.manage-devices` - Change input device ownership
- `io.github.hikaps.couchplay.manage-users` - Create/modify users
- `io.github.hikaps.couchplay.configure-audio` - Configure PipeWire

### D-Bus Interface
```
Service: io.github.hikaps.CouchPlayHelper
Object:  /io/github/hikaps/CouchPlayHelper
Interface: io.github.hikaps.CouchPlayHelper

Methods:
  - SetDeviceOwner(device_path: string, uid: uint32, gid: uint32) → bool
  - RestoreDeviceOwner(device_path: string) → bool
  - RestoreAllDevices() → void
  - CreateUser(username: string) → bool
  - ConfigurePipeWireMultiUser() → bool
```

### Installation
The helper is installed via `scripts/install-helper.sh` which:
1. Copies helper binary to `/usr/libexec/`
2. Installs Polkit policy to `/usr/share/polkit-1/actions/`
3. Installs D-Bus service files
4. Enables and starts the systemd service

## Implementation Phases

### Phase 1: Foundation (MVP)
- [x] Project plan
- [ ] Git repository setup
- [ ] CMake + Kirigami scaffolding
- [ ] Basic UI shell with navigation
- [ ] Device detection and listing
- [ ] Simple 2-instance horizontal split
- [ ] Gamescope process management
- [ ] Polkit helper for device ownership

### Phase 2: Core Features
- [ ] Full layout configuration UI
- [ ] Monitor detection and selection
- [ ] Session profiles (save/load)
- [ ] Linux user selection and creation
- [ ] PipeWire TCP auto-configuration

### Phase 3: Game Management
- [ ] Game library page
- [ ] Add games (Steam App ID / executable)
- [ ] Desktop shortcut creation
- [ ] Per-game session profiles

### Phase 4: Polish
- [ ] KDE panel hiding integration
- [ ] Error handling and user feedback
- [ ] Input remapper integration (optional)
- [ ] Flatpak packaging
- [ ] Documentation

### Phase 5: Extended Features (Future)
- [ ] More than 2 instances
- [ ] 2x2 grid layout
- [ ] Custom layout regions
- [ ] Controller hotplugging
- [ ] Steam account detection

## Git Workflow

```
main          ← stable releases only
  │
develop       ← integration branch
  │
  ├── feature/project-setup
  ├── feature/device-manager
  ├── feature/gamescope-wrapper
  ├── feature/polkit-helper
  ├── feature/ui-home
  ├── feature/ui-session-setup
  ├── feature/ui-device-assign
  ├── feature/profiles
  ├── feature/user-management
  ├── feature/game-library
  └── feature/flatpak
```

## Dependencies

### Build Dependencies (Fedora/Bazzite)
```bash
sudo dnf install cmake extra-cmake-modules \
    qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qtquickcontrols2-devel \
    kf6-kirigami-devel kf6-ki18n-devel kf6-kcoreaddons-devel \
    kf6-kconfig-devel kf6-kiconthemes-devel kf6-qqc2-desktop-style
```

### Runtime Dependencies
- gamescope
- steam
- PipeWire
- Polkit

## Configuration Files

### Session Profile Example
```ini
# ~/.config/couchplay/profiles/racing-night.conf
[General]
name=Racing Night
layout=horizontal

[Instance1]
user=hikaps
monitor=0
internalWidth=1920
internalHeight=1080
outputWidth=960
outputHeight=1080
refresh=60
devices=event3,event5
game=steam://rungameid/1551360

[Instance2]
user=player2
monitor=0
internalWidth=1920
internalHeight=1080
outputWidth=960
outputHeight=1080
refresh=60
devices=event4,event6
game=steam://rungameid/1551360
```

## References

- Original script: https://gist.github.com/NaviVani-dev/9a8a704a31313fd5ed5fa68babf7bc3a
- Gamescope: https://github.com/ValveSoftware/gamescope
- KDE Kirigami: https://develop.kde.org/docs/getting-started/kirigami/
- Polkit: https://www.freedesktop.org/software/polkit/docs/latest/
