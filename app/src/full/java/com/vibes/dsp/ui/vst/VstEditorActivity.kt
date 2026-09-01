/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * Licensed under GPL v3 — see app/src/main/cpp/plugin/IPlugin.h for full notice.
 */

package com.vibes.dsp.ui.vst

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import com.vibes.dsp.ui.layout.DisplayOrientation
import com.vibes.dsp.ui.layout.NnagaScreenGeometryProvider
import com.vibes.dsp.ui.layout.NnagaWindowPolicy
import com.vibes.dsp.ui.layout.screenSafePadding
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import com.vibes.dsp.engine.NativeEngine
import com.varcain.vsthost.NativeBridge
import com.varcain.vsthost.ui.PluginSurface
import com.vibes.dsp.ui.theme.NNAGATheme
import kotlinx.coroutines.delay

/**
 * Hosts a single VST plugin's wine-rendered editor surface. Runs in the
 * MAIN process (NOT a separate :vstui process) because the in-process X11
 * server lives in libvsthost.so which was loaded by the audio chain; a
 * separate process can't share it.
 *
 * Uses vsthost_lib's PluginSurface Composable for aspect-correct sizing
 * and touch routing. The X server letterboxes the plugin's framebuffer
 * inside the SurfaceView so the plugin appears centered with black bars.
 */
class VstEditorActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        NnagaWindowPolicy.install(this)
        val displayNumber = intent.getIntExtra(EXTRA_DISPLAY_NUMBER, -1)
        val pathId = intent.getLongExtra(EXTRA_PATH_ID, -1L)
        val pluginIndex = intent.getIntExtra(EXTRA_PLUGIN_INDEX, -1)
        if (displayNumber < 0 || pathId < 0L || pluginIndex < 0) {
            Log.e(TAG, "missing VST editor identity")
            finish(); return
        }
        Log.i(TAG, "onCreate display=$displayNumber path=$pathId plugin=$pluginIndex")

        NativeBridge.nativeStartX11Server(displayNumber, 4096, 2160)

        setContent {
            NNAGATheme {
                NnagaScreenGeometryProvider {
                    Surface(
                        modifier = Modifier.fillMaxSize(),
                        color = MaterialTheme.colorScheme.background
                    ) {
                        EditorScreen(displayNumber = displayNumber, pathId = pathId, pluginIndex = pluginIndex)
                    }
                }
            }
        }
    }

    companion object {
        private const val TAG = "VstEditorActivity"
        const val EXTRA_DISPLAY_NUMBER = "display_number"
        const val EXTRA_PATH_ID = "path_id"
        const val EXTRA_PLUGIN_INDEX = "plugin_index"

        fun intent(ctx: Context, displayNumber: Int, pathId: Long, pluginIndex: Int): Intent =
            Intent(ctx, VstEditorActivity::class.java).apply {
                putExtra(EXTRA_DISPLAY_NUMBER, displayNumber)
                putExtra(EXTRA_PATH_ID, pathId)
                putExtra(EXTRA_PLUGIN_INDEX, pluginIndex)
            }
    }
}

@androidx.compose.runtime.Composable
private fun EditorScreen(displayNumber: Int, pathId: Long, pluginIndex: Int) {
    var size by remember(pathId, pluginIndex) { mutableStateOf<Pair<Int, Int>?>(null) }
    LaunchedEffect(pathId, pluginIndex) {
        while (size == null) {
            val encoded = runCatching {
                NativeEngine.getInstance().nativeGetRackPluginEditorSize(pathId, pluginIndex)
            }.getOrDefault(0L)
            val w = (encoded ushr 32).toInt()
            val h = (encoded and 0xffffffffL).toInt()
            if (w > 0 && h > 0) {
                Log.i("VstEditorActivity", "editor size known: ${w}x$h")
                NativeBridge.nativeSetX11PluginSize(displayNumber, w, h)
                size = w to h
            } else {
                delay(250)
            }
        }
    }

    val s = size
    if (s != null) {
        PluginSurface(
            modifier = Modifier.fillMaxSize().screenSafePadding(),
            pluginWidth = s.first,
            pluginHeight = s.second,
            displayNumber = displayNumber,
            isVisible = true,
        )
    } else {
        Box(
            modifier = Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background),
            contentAlignment = Alignment.Center,
        ) {
            Text("Loading editor…", color = MaterialTheme.colorScheme.onSurface)
        }
    }
}
