/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of NNAGA.
 *
 * NNAGA is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * NNAGA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with NNAGA. If not, see <https://www.gnu.org/licenses/>.
 */

package com.vibes.dsp.ui.jsfx

internal sealed interface JsfxMenuEntry {
    data object Separator : JsfxMenuEntry

    data class Item(
        val id: Int,
        val label: String,
        val enabled: Boolean,
        val checked: Boolean,
        val children: List<JsfxMenuEntry> = emptyList(),
    ) : JsfxMenuEntry
}

/** Parses the upstream gfx_showmenu grammar while preserving its generated item IDs. */
internal fun parseJsfxMenu(spec: String): List<JsfxMenuEntry> {
    val segments = buildList {
        var start = 0
        while (start < spec.length) {
            val separator = spec.indexOf('|', start)
            if (separator < 0) {
                add(spec.substring(start))
                break
            }
            add(spec.substring(start, separator))
            start = separator + 1
        }
    }
    var position = 0
    var nextItemId = 1

    fun parseLevel(depth: Int): List<JsfxMenuEntry> {
        if (depth >= MAX_MENU_DEPTH) return emptyList()
        val entries = mutableListOf<JsfxMenuEntry>()
        while (position < segments.size) {
            val segment = segments[position++]
            var prefixEnd = 0
            var submenu = false
            var disabled = false
            var checked = false
            var endSubmenu = false
            while (prefixEnd < segment.length) {
                when (segment[prefixEnd]) {
                    '>' -> submenu = true
                    '#' -> disabled = true
                    '!' -> checked = true
                    '<' -> endSubmenu = true
                    else -> break
                }
                prefixEnd++
            }
            val label = segment.substring(prefixEnd)
            if (submenu) {
                val children = parseLevel(depth + 1)
                if (label.isNotEmpty() && children.isNotEmpty()) {
                    entries += JsfxMenuEntry.Item(
                        id = NO_ITEM_ID,
                        label = label,
                        enabled = !disabled,
                        checked = checked,
                        children = children,
                    )
                }
            } else if (label.isNotEmpty()) {
                entries += JsfxMenuEntry.Item(
                    id = nextItemId++,
                    label = label,
                    enabled = !disabled,
                    checked = checked,
                )
            } else if (!endSubmenu) {
                entries += JsfxMenuEntry.Separator
            }
            if (endSubmenu) break
        }
        return entries
    }

    return parseLevel(0)
}

private const val MAX_MENU_DEPTH = 8
private const val NO_ITEM_ID = 0
