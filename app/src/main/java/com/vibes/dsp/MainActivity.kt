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

package com.vibes.dsp

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import com.vibes.dsp.engine.AudioBackend
import com.vibes.dsp.engine.AudioSettingsManager
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.lifecycleScope
import com.vibes.dsp.engine.AudioEngine
import com.vibes.dsp.engine.EngineInitHelper
import com.vibes.dsp.engine.NativeEngine
import com.vibes.dsp.ui.components.NnagaButton
import com.vibes.dsp.ui.loading.PluginExtractScreen
import com.vibes.dsp.ui.navigation.AppNavigation
import com.vibes.dsp.ui.theme.NNAGATheme
import com.vibes.dsp.ui.tone3000.Tone3000CallbackHandler
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.io.FileNotFoundException
import java.io.FileOutputStream
import java.io.InputStream

class MainActivity : ComponentActivity() {
    private val audioPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { }

    private sealed interface StartupState {
        data object Initializing : StartupState
        data object Ready : StartupState
        data class Failed(val message: String) : StartupState
    }

    private var startupState by mutableStateOf<StartupState>(StartupState.Initializing)
    private var extractedCount by mutableIntStateOf(0)
    private var extractTotalCount by mutableIntStateOf(0)

    override fun onNewIntent(intent: android.content.Intent?) {
        super.onNewIntent(intent)
        handleAuthIntent(intent)
    }

    private fun handleAuthIntent(intent: android.content.Intent?) {
        intent?.data?.let { uri ->
            if (uri.scheme == "guitarrackcraft") {
                when (uri.host) {
                    "tone3000auth" -> {
                        val apiKey = uri.getQueryParameter("api_key")
                        if (apiKey != null) {
                            android.util.Log.d("MainActivity", "Received Tone3000 api_key")
                            Tone3000CallbackHandler.onApiKeyReceived(apiKey)
                        }
                    }
                    "tone3000select" -> {
                        val toneUrl = uri.getQueryParameter("tone_url")
                        if (toneUrl != null) {
                            android.util.Log.d("MainActivity", "Received Tone3000 tone_url")
                            Tone3000CallbackHandler.onToneUrlReceived(toneUrl)
                        }
                    }
                }
            }
        }
    }


    override fun onCreate(savedInstanceState: Bundle?) {
        NativeEngine.getInstance().nativeApplyCurrentThreadUiAffinity()
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        if (AudioSettingsManager.getAudioBackend(this) == AudioBackend.AndroidOboe &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) !=
                PackageManager.PERMISSION_GRANTED
        ) {
            audioPermissionLauncher.launch(Manifest.permission.RECORD_AUDIO)
        }
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val display = getSystemService(WindowManager::class.java).defaultDisplay
        val sixtyHertzMode = display.supportedModes.firstOrNull {
            kotlin.math.abs(it.refreshRate - 60f) < 0.5f
        }
        window.attributes = window.attributes.apply {
            preferredRefreshRate = 60f
            if (sixtyHertzMode != null) {
                preferredDisplayModeId = sixtyHertzMode.modeId
            }
        }

        // Handle auth callback if activity started via deep link
        handleAuthIntent(intent)

        // Full-screen immersive: hide system bars, draw behind cutout
        WindowCompat.setDecorFitsSystemWindows(window, false)
        val controller = WindowInsetsControllerCompat(window, window.decorView)
        controller.hide(WindowInsetsCompat.Type.systemBars())
        controller.systemBarsBehavior =
            WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE

        startEngineInitialization()

        setContent {
            NNAGATheme {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background
                ) {
                    when (val state = startupState) {
                        StartupState.Initializing -> PluginExtractScreen(
                            extracted = extractedCount.takeIf { extractTotalCount > 0 },
                            total = extractTotalCount.takeIf { it > 0 }
                        )
                        StartupState.Ready -> AppNavigation(engineReady = true)
                        is StartupState.Failed -> StartupFailureScreen(
                            message = state.message,
                            onRetry = { startEngineInitialization() }
                        )
                    }
                }
            }
        }
    }
    private fun startEngineInitialization() {
        lifecycleScope.launch {
            startupState = StartupState.Initializing
            extractedCount = 0
            extractTotalCount = 0
            try {
                val initialized = withContext(Dispatchers.IO) {
                    // VST/Wine staging is optional background work. It can be slow
                    // on first launch or while repairing an imported prefix, but
                    // must never prevent the core rack UI and audio engine starting.
                    prepareLv2AndInitEngine()
                }
                if (initialized) {
                    startupState = StartupState.Ready
                    refreshPluginRegistryAfterBackgroundSetup()
                    maybeRunAhbSpike()
                    maybeAutostartEditor()
                } else {
                    startupState = StartupState.Failed(
                        "Native audio engine failed to initialize. Check the setup logs and retry."
                    )
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (t: Throwable) {
                android.util.Log.e("MainActivity", "Startup initialization failed", t)
                startupState = StartupState.Failed(
                    "Startup failed: ${t.message ?: "unexpected initialization error"}. Retry."
                )
            }
        }
    }

    /**
     * A full-flavor VST setup starts from Application.onCreate. Wait for it only
     * after the native engine exists, then refresh its plugin factories. This
     * keeps a corrupt VST prefix or long first-run extraction from blocking the
     * core UI forever.
     */
    private fun refreshPluginRegistryAfterBackgroundSetup() {
        val prerequisite = applicationContext as? StartupPrerequisite ?: return
        lifecycleScope.launch(Dispatchers.IO) {
            try {
                if (prerequisite.awaitStartupPrerequisite()) {
                    NativeEngine.getInstance().nativeRefreshPluginRegistry()
                    android.util.Log.i(
                        "MainActivity",
                        "Refreshed plugin registry after background VST setup"
                    )
                } else {
                    android.util.Log.w(
                        "MainActivity",
                        "Background VST setup failed; core engine remains available"
                    )
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (t: Throwable) {
                android.util.Log.e(
                    "MainActivity",
                    "Background VST setup failed; core engine remains available",
                    t
                )
            }
        }
    }


    private fun extractLV2Assets() {
        // Extract LV2 plugins from assets to internal storage (use applicationContext for consistency with native path)
        val lv2Dir = File(applicationContext.filesDir, "lv2")
        lv2Dir.mkdirs()

        try {
            // Primary: use build-time file manifest for reliable extraction.
            // assets.list() is unreliable across split APKs on some Android versions,
            // causing entire bundles to be extracted as empty directories.
            val fileList = try {
                assets.open("lv2_files.txt").bufferedReader().use { reader ->
                    reader.readLines().map { it.trim() }.filter { it.isNotEmpty() && !it.endsWith(".so") }
                }
            } catch (_: Exception) { null }

            if (fileList != null) {
                android.util.Log.d("MainActivity", "Extracting ${fileList.size} LV2 asset files from lv2_files.txt")
                for (relPath in fileList) {
                    val outputFile = File(lv2Dir, relPath)
                    outputFile.parentFile?.mkdirs()
                    try {
                        assets.open("lv2/$relPath").use { input ->
                            FileOutputStream(outputFile).use { output ->
                                input.copyTo(output)
                            }
                        }
                    } catch (e: Exception) {
                        android.util.Log.w("MainActivity", "Failed to extract lv2/$relPath: ${e.message}")
                    }
                }
            } else {
                // Fallback: enumerate via assets.list() (full variant / no manifest)
                val bundleNames = assets.list("lv2")?.toList() ?: emptyList()
                android.util.Log.d("MainActivity", "lv2_files.txt not found, assets.list returned ${bundleNames.size} entries")
                bundleNames.forEach { assetName ->
                    val assetPath = "lv2/$assetName"
                    val outputDir = File(lv2Dir, assetName)
                    outputDir.mkdirs()
                    copyAssetRecursive(assetPath, outputDir)
                }
            }
            val bundleCount = lv2Dir.list()?.size ?: 0
            android.util.Log.d("MainActivity", "LV2 assets extracted to ${lv2Dir.absolutePath} ($bundleCount top-level entries)")
        } catch (e: Exception) {
            android.util.Log.e("MainActivity", "Failed to extract LV2 assets", e)
        }
    }
    
    private fun copyAssetRecursive(assetPath: String, outputDir: File) {
        try {
            outputDir.mkdirs()
            
            // Try to list as directory first
            val assetList = assets.list(assetPath)
            if (assetList != null && assetList.isNotEmpty()) {
                // It's a directory - recurse into it
                assetList.forEach { item ->
                    val newAssetPath = "$assetPath/$item"
                    val newOutputDir = File(outputDir, item)
                    copyAssetRecursive(newAssetPath, newOutputDir)
                }
            } else {
                // list() returned null or empty - try to open as file
                try {
                    val inputStream: InputStream = assets.open(assetPath)
                    val fileName = assetPath.substringAfterLast('/')

                    // Skip .so files — plugin binaries are loaded from nativeLibDir
                    if (fileName.endsWith(".so")) {
                        inputStream.close()
                        return
                    }
                    val outputFile = File(outputDir.parentFile ?: outputDir, fileName)
                    
                    // Remove existing file/directory if it exists
                    if (outputFile.exists()) {
                        if (outputFile.isDirectory) {
                            outputFile.deleteRecursively()
                        } else {
                            outputFile.delete()
                        }
                    }
                    
                    outputFile.parentFile?.mkdirs()
                    FileOutputStream(outputFile).use { output ->
                        inputStream.copyTo(output)
                    }
                    inputStream.close()
                } catch (e: FileNotFoundException) {
                    // Asset might not exist, skip
                    android.util.Log.d("MainActivity", "Asset not found as file: $assetPath")
                }
            }
        } catch (e: Exception) {
            android.util.Log.e("MainActivity", "Failed to copy asset: $assetPath", e)
        }
    }

    /**
     * Debug autostart: if cache/autostart_editor.txt contains a plugin name (or
     * id substring), open that plugin's X11 editor on launch — a faithful BIAS
     * FX 2 black-editor repro with no manual rack interaction, so the FEX stack
     * recursion can be iterated on without anyone driving the device. Marker
     * absent (the normal case) = no-op. Drop the marker with:
     *   adb shell run-as <pkg> sh -c 'echo "BIAS FX 2" > cache/autostart_editor.txt'
     */
    /** Phase 0 GPU-upgrade spike (throwaway). If cache/ahbspike exists, run the
     *  cross-driver AHardwareBuffer + fence interop test in-process (the app has
     *  GPU access + adrenotools works here, same as the wine subprocess) and log
     *  PASS/FAIL to logcat tag "AhbSpike". Drop the marker with:
     *    adb shell run-as <pkg> sh -c 'printf x > cache/ahbspike'
     */
    private suspend fun maybeRunAhbSpike() {
        try {
            if (!File(cacheDir, "ahbspike").exists()) return
            val turnipDir = File(filesDir, "wine/turnip").absolutePath + "/"  // trailing slash required
            val logPath = File(cacheDir, "ahbspike.log").absolutePath
            withContext(Dispatchers.IO) {
                android.util.Log.i("AhbSpike", "running spike (hook=${applicationInfo.nativeLibraryDir} turnip=$turnipDir log=$logPath)")
                val bridgeClass = Class.forName("com.varcain.vsthost.NativeBridge")
                val method = bridgeClass.getMethod(
                    "nativeAhbSpike",
                    String::class.java, String::class.java, String::class.java, String::class.java
                )
                val ok = method.invoke(
                    null, applicationInfo.nativeLibraryDir, turnipDir, "vulkan.ad07xx.so", logPath
                ) as Boolean
                android.util.Log.i("AhbSpike", "spike returned ok=$ok")
            }
        } catch (e: Throwable) {
            android.util.Log.e("AhbSpike", "spike threw", e)
        }
    }

    private suspend fun maybeAutostartEditor() {
        try {
            val marker = File(cacheDir, "autostart_editor.txt")
            if (!marker.exists()) return
            val want = marker.readText().trim()
            if (want.isEmpty()) return
            val rm = com.vibes.dsp.engine.RackManager
            val match = withContext(Dispatchers.IO) {
                rm.getAvailablePlugins().firstOrNull {
                    it.name.contains(want, ignoreCase = true) ||
                    it.fullId.contains(want, ignoreCase = true) ||
                    it.id.contains(want, ignoreCase = true)
                }
            }
            if (match == null) {
                android.util.Log.w("Autostart", "no plugin matches '$want'")
                return
            }
            // Re-add to the MAIN rack (audio chain) so it behaves like a restored
            // session, not just the editor window. Guard on empty rack so config
            // changes / re-creates don't stack duplicates. Heavy (loads plugin) -> IO.
            // Debug toggle: cache/autostart_norack present => editor only (clean,
            // single vst_host log for pipe tracing). Absent (normal) => add to rack.
            if (!File(cacheDir, "autostart_norack").exists()) {
                withContext(Dispatchers.IO) {
                    val trackId = rm.getTracks().firstOrNull()?.id
                    if (trackId == null) {
                        android.util.Log.w("Autostart", "no rack track available")
                    } else if (rm.getRackSize(trackId) == 0) {
                        val pos = rm.addPlugin(trackId, match.fullId)
                        android.util.Log.i("Autostart", "added ${match.name} to rack at pos=$pos")
                    } else {
                        android.util.Log.i("Autostart", "rack already has ${rm.getRackSize(trackId)} plugin(s), skip add")
                    }
                }
            }
            android.util.Log.i("Autostart", "opening editor for ${match.name} (${match.fullId})")
            startActivity(
                android.content.Intent(this@MainActivity, X11PluginUIActivity::class.java)
                    .putExtra(X11PluginUIActivity.EXTRA_PLUGIN_ID, match.fullId)
            )
        } catch (e: Exception) {
            android.util.Log.e("Autostart", "failed", e)
        }
    }

    private fun prepareLv2AndInitEngine(): Boolean {
        if (EngineInitHelper.isInitialized) {
            android.util.Log.d("MainActivity", "Native engine already initialized; skipping LV2 extraction")
            return true
        }
        EngineInitHelper.preloadLilv(applicationInfo.nativeLibraryDir)
        extractLV2Assets()
        return EngineInitHelper.initEngine(this) { extracted, total ->
            extractedCount = extracted
            extractTotalCount = total
        }
    }

    @Composable
    private fun StartupFailureScreen(message: String, onRetry: () -> Unit) {
        Box(
            modifier = Modifier.fillMaxSize(),
            contentAlignment = Alignment.Center
        ) {
            Column(
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(16.dp),
                modifier = Modifier.padding(24.dp)
            ) {
                Text(message, color = MaterialTheme.colorScheme.onBackground)
                NnagaButton(onClick = onRetry) {
                    Text("Retry")
                }
            }
        }
    }


    override fun onPause() {
        super.onPause()
        android.util.Log.i("AudioLifecycle", "MainActivity.onPause (isFinishing=$isFinishing)")
    }

    override fun onStop() {
        super.onStop()
        android.util.Log.i("AudioLifecycle", "MainActivity.onStop (isFinishing=$isFinishing)")
    }

    override fun onDestroy() {
        super.onDestroy()
        // Only stop the engine when the activity is really finishing (user left the app).
        // When isFinishing() is false, the activity is being recreated (e.g. config change);
        // don't stop the engine so the new instance keeps the same running stream.
        android.util.Log.i("AudioLifecycle", "MainActivity.onDestroy (isFinishing=$isFinishing)")
        if (isFinishing()) {
            android.util.Log.i("AudioLifecycle", "MainActivity.onDestroy (finishing) -> stopEngine()")
            AudioEngine.stop()
        }
    }
}
