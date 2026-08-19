/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * Licensed under GPL v3 — see app/src/main/cpp/plugin/IPlugin.h for full notice.
 */

package com.vibes.dsp.ui.vst

import androidx.compose.runtime.Composable

@Composable
fun VstManagerTab(onWineSessionActiveChanged: (Boolean) -> Unit) {
    VstManagerScreen(
        onNavigateBack = {},
        embedded = true,
        onWineSessionActiveChanged = onWineSessionActiveChanged,
    )
}
