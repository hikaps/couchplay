# AGENTS.md - QML Layer Guidelines for CouchPlay

**QML module "io.github.hikaps.couchplay 1.0" - Kirigami-based UI (GPL-3.0-or-later)**

## OVERVIEW

QML/Kirigami UI layer: 13 files (~9.7K lines), 8 pages, 4 components.

## STRUCTURE

**Pages (pages/):** HomePage, SessionSetupPage, DeviceAssignmentPage, ProfilesPage, UsersPage, SettingsPage (612 lines)

**Components (components/):** SelectableCard (selection state), ActionCard (CTA with badge), InfoCard, PresetSelector (ComboBox)

**Dialogs (components/dialogs/):** AddPresetDialog (search/filter ListView), EditPresetDialog (ListModel + FolderDialog), DeletePresetDialog, ResetSettingsDialog, InstallHelperDialog

## WHERE TO LOOK

| Task | Location | Notes |
|------|----------|-------|
| Manager injection | Main.qml | Creates all backend instances |
| Page navigation | Main.qml | Helper functions push pages with props |
| Form layouts | SettingsPage.qml | Kirigami.FormLayout patterns |
| Custom delegates | PresetSelector.qml | ComboBox with icons |
| Hover effects | SelectableCard.qml | HoverHandler + animations |
| Audio settings UI | SettingsPage.qml:468-522 | 3-state audio detection (no server / detected / configured) |
| Session launch | HomePage.qml | Gamescope availability check, session start/stop buttons |

## CONVENTIONS

**Property Injection:**
- Main.qml creates manager instances as properties
- Pages declare `required property var managerName`
- Navigation: `pageStack.push(pageComponent, { manager: instance })`

**Computed Properties:** Extract repeated null checks into named bool properties.
- Example: `property bool audioServerDetected: root.audioManager && root.audioManager.audioServer !== ""`
- Use in bindings instead of re-checking the same condition 5 times (see SettingsPage audio section)

**Component Design:**
- Use `Kirigami.AbstractCard` (not Kirigami.Card) to avoid clipping
- Document with `/** */` usage block
- `default property alias content: container.data` for slots
- Hover via `HoverHandler`, not MouseArea
- Signals: `clicked()`, `presetSelected(string id)`

## ANTI-PATTERNS

- QtQuick 1.x: Use QtQuick 2.x (Qt6)
- Inline styling: Use Kirigami.Theme, not hardcoded colors
- MouseArea for hover: Use HoverHandler (non-blocking)
- Kirigami.Card: Always use AbstractCard
- String i18n: Use i18nc with context, not + operator
- Page stacking: Use Main.qml helpers, don't pageStack.push() directly
- Repeated null checks: Extract to computed property (e.g., `audioServerDetected`)

## UNIQUE PATTERNS

**Page Components:** Main.qml defines pages as Components to defer instantiation (memory optimization).

**Null Safety:** `property bool helperAvailable: helperClient?.available ?? false` uses optional chaining.

**Lazy Managers:** SettingsPage creates internal PresetManager if not provided (enables standalone testing).

**Badge:** ActionCard/SelectableCard support `badgeCount` with visual indicator.

## DIALOG PATTERNS

**Structure:**
- Base: `Kirigami.Dialog` (complex) or `Kirigami.PromptDialog` (simple confirmations)
- Size: `preferredWidth: Kirigami.Units.gridUnit * 30`
- Buttons: `standardButtons: Kirigami.Dialog.Yes | Kirigami.Dialog.Cancel`
- Injection: `required property var managerName`

**I18n Context Tags:**
- `@title:dialog` — Dialog titles
- `@info` — Explanatory text
- `@action:button` — Button labels

**Common Patterns:** `applicationWindow().showPassiveNotification()` for toasts, `Kirigami.PlaceholderMessage` for empty states, `footerLeadingComponent` for footer actions.
