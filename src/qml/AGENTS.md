# AGENTS.md - QML Layer Guidelines for CouchPlay

**QML module "io.github.hikaps.couchplay 1.0" - Kirigami-based UI (GPL-3.0-or-later)**

## OVERVIEW

QML/Kirigami UI layer: 17 files (~3.7K lines), 6 pages, 5 components, 5 dialogs.

## STRUCTURE

**Pages (pages/):** HomePage (325 lines), SessionSetupPage (968 lines), DeviceAssignmentPage (479 lines), UsersPage (404 lines), SettingsPage (729 lines), ProfilesPage (256 lines)

**Components (components/):** SelectableCard (selection state + hover), ActionCard (CTA with badge), InfoCard (header + content slot), PresetSelector (ComboBox with icons), CollapsibleSection (animated toggle)

**Dialogs (components/dialogs/):** AddPresetDialog (search/filter ListView), EditPresetDialog (ListModel + FolderDialog), DeletePresetDialog, ResetSettingsDialog, InstallHelperDialog

## WHERE TO LOOK

| Task | Location | Notes |
|------|----------|-------|
| Manager injection | Main.qml | Creates 11 backend instances, cross-manager signal connections |
| Page navigation | Main.qml | Helper functions push pages with props (single-page, clear stack) |
| Navigation graph | Main.qml:139-165 | GlobalDrawer: Home → SessionSetup → Profiles → Users → Settings |
| Device drag-and-drop | DeviceAssignmentPage.qml | Drag.keys = "application/x-couchplay-device", DropArea per player |
| Form layouts | SettingsPage.qml | 10+ Kirigami.FormLayout groups with conditional visibility |
| Audio 3-state | SettingsPage.qml | No server / detected / configured (color-coded InlineMessage) |
| Session config | SessionSetupPage.qml | Layout cards, instance Repeater, CollapsibleSection for advanced |
| Custom delegates | PresetSelector.qml | ComboBox with icons and "Built-in" badge |

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
