// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

/**
 * D-Bus API level reported by CouchPlayHelper::GetVersionInfo().
 *
 * Bump whenever a change lands that a GUI built against the new interface
 * cannot tolerate from an older helper (new required slot, changed argument
 * or semantics). Helpers older than 0.6.1 predate GetVersionInfo() and are
 * treated by the GUI as level 0.
 */
constexpr int COUCHPLAY_HELPER_API_LEVEL = 1;
