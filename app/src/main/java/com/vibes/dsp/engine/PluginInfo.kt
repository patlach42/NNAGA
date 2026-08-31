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

package com.vibes.dsp.engine

/**
 * One enumeration/scale point for a control port (label + value).
 */
data class ScalePoint(
    val label: String = "",
    val value: Float = 0.0f
)

/**
 * Information about a plugin port (parameter or audio).
 */
data class PortInfo(
    val index: Int = 0,
    val name: String = "",
    val symbol: String = "",
    val isInput: Boolean = true,
    val isAudio: Boolean = false,
    val isControl: Boolean = false,
    val isToggle: Boolean = false,
    val defaultValue: Float = 0.0f,
    val minValue: Float = 0.0f,
    val maxValue: Float = 1.0f,
    val scalePoints: List<ScalePoint> = emptyList(),
    val unit: String = "",
    val stepCount: Int = 0,
    val isReadOnly: Boolean = false
)

/**
 * Information about an available plugin.
 */
/**
 * Describes the preferred UI rendering mode for a plugin.
 */
enum class UiType(val displayName: String) {
    /** Native X11 UI (xputty/Cairo based). */
    X11("Native"),
    /** Web-based modgui rendered in a WebView. */
    MODGUI("Modgui"),
    /** In-process JSFX graphics rendered by ysfx. */
    JSFX("JSFX"),
    /** Plain Compose sliders / toggles (always available). */
    SLIDERS("Sliders")
}

data class PluginInfo(
    val id: String = "",
    val name: String = "",
    val format: String = "",
    val originPath: String = "",
    val ports: List<PortInfo> = emptyList(),
    val modguiBasePath: String = "",
    val modguiIconTemplate: String = "",
    val hasX11Ui: Boolean = false,
    val x11UiBinaryPath: String = "",
    val x11UiUri: String = "",
    val thumbnailPath: String = "",
    val description: String = "",
    val arch: String = "",
    val parameterMetadataRevision: Long = 1L
) {
    val fullId: String
        get() = "$format:$id"
    
    val controlPorts: List<PortInfo>
        get() = ports.filter { it.isControl && !it.isAudio }
    
    val hasModgui: Boolean
        get() = modguiBasePath.isNotEmpty() && modguiIconTemplate.isNotEmpty()

    /** Default UI: generic sliders for VST, then Modgui/X11 for native formats. */
    val preferredUiType: UiType
        get() = when {
            format == "VST2" || format == "VST3" -> UiType.SLIDERS
            hasModgui -> UiType.MODGUI
            hasX11Ui -> UiType.X11
            else -> UiType.SLIDERS
        }

    /** List of GUI types this plugin exposes (X11, MODGUI, SLIDERS). Sliders are always available. */
    val guiTypes: List<UiType>
        get() = buildList {
            if (hasX11Ui) add(UiType.X11)
            if (hasModgui) add(UiType.MODGUI)
            add(UiType.SLIDERS)
        }
}
