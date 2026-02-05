# AGENTS.md - QML Layer Guidelines for CouchPlay

**QML module "io.github.hikaps.couchplay 1.0" - Kirigami-based UI (GPL-3.0-or-later)**

## OVERVIEW

QML/Kirigami UI layer: 13 files (~9.7K lines), 8 pages, 4 components.

## STRUCTURE

**Pages (pages/):** HomePage, SessionSetupPage, DeviceAssignmentPage, LayoutPage, ProfilesPage, GamesPage, UsersPage, SettingsPage (1023 lines)

**Components (components/):** SelectableCard (selection state), ActionCard (CTA with badge), InfoCard, PresetSelector (ComboBox)

## WHERE TO LOOK

| Task | Location | Notes |
|------|----------|-------|
| Manager injection | Main.qml | Creates all backend instances |
| Page navigation | Main.qml | Helper functions push pages with props |
| Form layouts | SettingsPage.qml | Kirigami.FormLayout patterns |
| Custom delegates | PresetSelector.qml | ComboBox with icons |
| Hover effects | SelectableCard.qml | HoverHandler + animations |
| Theme system | All files | Kirigami.Theme properties |

## CONVENTIONS

**Property Injection:**
- Main.qml creates manager instances as properties
- Pages declare `required property var managerName`
- Navigation: `pageStack.push(pageComponent, { manager: instance })`

**Component Design:**
- Use `Kirigami.AbstractCard` (not Kirigami.Card) to avoid clipping
- Document with `/** */` usage block
- `default property alias content: container.data` for slots
- Hover via `HoverHandler`, not MouseArea
- Signals: `clicked()`, `presetSelected(string id)`

**Layout & Spacing:**
- `Kirigami.Units` for sizing (gridUnit, largeSpacing, smallSpacing)
- Forms: `Kirigami.FormLayout` with `Kirigami.FormData.isSection: true`
- Responsive: `wideMode: root.width > Kirigami.Units.gridUnit * 30`
- Grid: `Math.floor(root.width / (Kirigami.Units.gridUnit * 16))`

**Kirigami Components:**
- Page: `Kirigami.ScrollablePage` with `title: i18nc("@title", "...")`
- Nav: `Kirigami.GlobalDrawer` in `globalDrawer`
- Cards: `Kirigami.AbstractCard` (never Kirigami.Card)
- Messages: `Kirigami.PlaceholderMessage`, `Kirigami.InlineMessage`, `Kirigami.Chip`

**Styling:**
- Colors: `Kirigami.Theme.backgroundColor`, `Kirigami.Theme.highlightColor`, `Kirigami.Theme.hoverColor`
- Animations: `Behavior on color { ColorAnimation { duration: Kirigami.Units.shortDuration } }`
- Secondary text opacity: `0.7`

## ANTI-PATTERNS

- QtQuick 1.x: Use QtQuick 2.x (Qt6)
- Inline styling: Use Kirigami.Theme, not hardcoded colors
- MouseArea for hover: Use HoverHandler (non-blocking)
- Kirigami.Card: Always use AbstractCard
- String i18n: Use i18nc with context, not + operator
- Page stacking: Use Main.qml helpers, don't pageStack.push() directly

## UNIQUE PATTERNS

**Page Components:** Main.qml defines pages as Components to defer instantiation (memory optimization).

**Null Safety:** `property bool helperAvailable: helperClient?.available ?? false` uses optional chaining.

**Lazy Managers:** SettingsPage creates internal PresetManager if not provided (enables standalone testing).

**Badge:** ActionCard/SelectableCard support `badgeCount` with visual indicator.
