/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of Guitar RackCraft.
 *
 * Guitar RackCraft is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Guitar RackCraft is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Guitar RackCraft. If not, see <https://www.gnu.org/licenses/>.
 */

package com.vibes.dsp.ui.theme

import android.app.Activity
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalView
import androidx.core.view.WindowCompat

private val AmoledColorScheme = darkColorScheme(
    primary = Color(0xFFB6F43B),
    onPrimary = Color(0xFF182000),
    primaryContainer = Color(0xFF344900),
    onPrimaryContainer = Color(0xFFD0FF75),
    inversePrimary = Color(0xFF4F6500),
    secondary = Color(0xFFC4D7A0),
    onSecondary = Color(0xFF29351B),
    secondaryContainer = Color(0xFF3D4B29),
    onSecondaryContainer = Color(0xFFDFF5B9),
    tertiary = Color(0xFFB9CCE8),
    onTertiary = Color(0xFF233143),
    tertiaryContainer = Color(0xFF39495C),
    onTertiaryContainer = Color(0xFFD5E3FC),
    background = Color(0xFF000000),
    onBackground = Color(0xFFF5F5F5),
    surface = Color(0xFF0A0A0A),
    onSurface = Color(0xFFF5F5F5),
    surfaceVariant = Color(0xFF121212),
    onSurfaceVariant = Color(0xFFC5C8BD),
    outline = Color(0xFF8F9387),
    outlineVariant = Color(0xFF44473F),
    inverseSurface = Color(0xFFE2E3D8),
    inverseOnSurface = Color(0xFF2F312B),
    error = Color(0xFFFFB4AB),
    onError = Color(0xFF690005),
    errorContainer = Color(0xFF93000A),
    onErrorContainer = Color(0xFFFFDAD6)
)

@Composable
fun GuitarRackCraftTheme(
    @Suppress("UNUSED_PARAMETER") darkTheme: Boolean = true,
    content: @Composable () -> Unit
) {
    val view = LocalView.current
    if (!view.isInEditMode) {
        SideEffect {
            val window = (view.context as Activity).window
            window.statusBarColor = Color.Black.toArgb()
            window.navigationBarColor = Color.Black.toArgb()
            WindowCompat.getInsetsController(window, view).apply {
                isAppearanceLightStatusBars = false
                isAppearanceLightNavigationBars = false
            }
        }
    }

    MaterialTheme(
        colorScheme = AmoledColorScheme,
        typography = androidx.compose.material3.Typography(),
        content = content
    )
}
