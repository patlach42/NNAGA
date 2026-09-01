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

import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Choreographer
import android.widget.FrameLayout
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import androidx.activity.ComponentActivity
import androidx.core.view.ViewCompat
import com.vibes.dsp.engine.EngineInitHelper
import com.vibes.dsp.ui.layout.DisplayOrientation
import com.vibes.dsp.ui.layout.GeometryCachePolicy
import com.vibes.dsp.ui.layout.NnagaWindowPolicy
import com.vibes.dsp.ui.layout.ScreenGeometryStore
import com.vibes.dsp.engine.RackManager
import com.vibes.dsp.engine.X11Bridge
import com.vibes.dsp.ui.x11.X11DisplayManager
import com.vibes.dsp.ui.layout.ScreenGeometryObserver

/**
 * Hosts the X11 plugin UI in a **separate process** (:x11ui) so EGL/GL state is not shared
 * with the main process HWUI, avoiding "pthread_mutex_lock on destroyed mutex" crashes.
 *
 * Started by the main app with EXTRA_PLUGIN_ID (plugin fullId, e.g. "LV2:http://..."). This
 * process inits its own native engine (no audio), adds the plugin to the rack, and runs
 * createPluginUI on a Surface in this activity. Parameter changes are not yet synced back
 * to the main process.
 */
class X11PluginUIActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        NnagaWindowPolicy.install(
            this,
            intent.getStringExtra(EXTRA_ORIENTATION)?.let(DisplayOrientation::fromPersisted),
        )
        pluginId = intent.getStringExtra(EXTRA_PLUGIN_ID)
        if (pluginId.isNullOrEmpty()) {
            Log.e(TAG, "Missing $EXTRA_PLUGIN_ID")
            finish()
            return
        }
        Log.i(TAG, "onCreate process=${android.os.Process.myPid()} pluginId=$pluginId")
        val root = FrameLayout(this).apply {
            setBackgroundColor(android.graphics.Color.BLACK)
        }
        val surfaceView = SurfaceView(this).apply {
            visibility = View.GONE
            setOnTouchListener { _, event ->
                val action = when (event.action) {
                    android.view.MotionEvent.ACTION_DOWN -> 0
                    android.view.MotionEvent.ACTION_UP -> 1
                    android.view.MotionEvent.ACTION_MOVE -> 2
                    else -> -1
                }
                if (action >= 0 && displayNumber >= 0) {
                    if (++touchLogCount <= 40 || touchLogCount % 50 == 0) {
                        Log.i(TAG, "X11Touch display=$displayNumber action=$action x=${event.x.toInt()} y=${event.y.toInt()} (#$touchLogCount)")
                    }
                    X11Bridge.injectTouch(displayNumber, action, event.x.toInt(), event.y.toInt())
                    X11DisplayManager.pluginUiExecutor.execute {
                        X11Bridge.idlePluginUIs()
                        X11Bridge.requestX11Frame(displayNumber)
                    }
                }
                true
            }
        }
        surfaceView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {}
            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                if (attached || width < 10 || height < 10) {
                    if (width > 0 && height > 0 && displayNumber >= 0) X11Bridge.setSurfaceSize(displayNumber, width, height)
                    return
                }
                attached = true
                Log.i(TAG, "surfaceChanged ${width}x$height -> attach and createPluginUI")
                val rootId = X11Bridge.attachSurfaceToDisplay(displayNumber, holder.surface, width, height)
                if (rootId == 0L) {
                    Log.e(TAG, "attachSurfaceToDisplay failed")
                    finish()
                    return
                }
                X11Bridge.beginCreatePluginUI(pathId, pluginIndex, pluginInstanceId, uiInstanceId, displayNumber)
                X11DisplayManager.pluginUiExecutor.execute {
                    X11Bridge.ensureX11LibsDir(this@X11PluginUIActivity)
                    Thread.sleep(X11_INIT_DELAY_MS)
                    val ok = X11Bridge.createPluginUI(pathId, pluginIndex, pluginInstanceId, uiInstanceId, displayNumber, rootId)
                    Handler(Looper.getMainLooper()).post {
                        if (!ok) {
                            Log.e(TAG, "createPluginUI failed")
                            finish()
                        } else {
                            startFrameAndIdleLoops()
                        }
                    }
                }
            }
            override fun surfaceDestroyed(holder: SurfaceHolder) {
                if (attached) {
                    attached = false
                    X11Bridge.signalDetachSurfaceFromDisplay(displayNumber)
                    pendingDetach = true
                    Log.i(TAG, "surfaceDestroyed -> deferred cleanup")
                }
            }
        })
        surfaceView.contentDescription = "x11_plugin_viewport"
        root.addView(surfaceView, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.MATCH_PARENT,
        ))
        geometryObserver = ScreenGeometryObserver(
            root,
            com.vibes.dsp.ui.layout.ScreenGeometryStore(this, GeometryCachePolicy.NoCache),
        ) { geometry ->
            if (geometry.authoritative) {
                val safe = geometry.safeInsets()
                val params = surfaceView.layoutParams as FrameLayout.LayoutParams
                params.setMargins(safe.left, safe.top, safe.right, safe.bottom)
                surfaceView.layoutParams = params
                surfaceView.visibility = View.VISIBLE
                surfaceView.isEnabled = true
            }
        }.also { it.start() }
        setContentView(root)
    }

    private fun initNativeInThisProcess() {
        EngineInitHelper.preloadLilv(applicationInfo.nativeLibraryDir)
        if (!EngineInitHelper.initEngine(this)) {
            Log.e(TAG, "nativeInit failed")
            finish()
            return
        }
        pathId = RackManager.getTracks().firstOrNull()?.id ?: run {
            Log.e(TAG, "No track available")
            finish()
            return
        }
        pluginIndex = RackManager.addPlugin(pathId, pluginId!!, 0)
        if (pluginIndex < 0) {
            Log.e(TAG, "addPluginToRack failed for $pluginId")
            finish()
            return
        }
        pluginInstanceId = RackManager.getRackPluginInstanceId(pathId, pluginIndex)
        uiInstanceId = System.nanoTime().coerceAtLeast(1L)
        displayNumber = X11DisplayManager.allocateDisplay()
        if (displayNumber < 0) {
            Log.e(TAG, "No X11 display available")
            finish()
            return
        }
        Log.i(TAG, "Subprocess inited: plugin at path=$pathId index=$pluginIndex instance=$pluginInstanceId, display $displayNumber")
    }

    override fun onDestroy() {
        geometryObserver?.stop()
        stopFrameAndIdleLoops()
        if (pendingDetach && displayNumber >= 0) {
            X11Bridge.detachAndDestroyX11DisplayIfExists(displayNumber)
            X11DisplayManager.releaseDisplay(displayNumber)
        }
        super.onDestroy()
    }

    /** Pump EGL swaps and plugin idle so the X11 UI is visible and responsive. */
    private fun startFrameAndIdleLoops() {
        if (displayNumber < 0) return
        idleScheduled = true
        val choreographer = Choreographer.getInstance()
        frameCallback = object : Choreographer.FrameCallback {
            override fun doFrame(frameTimeNanos: Long) {
                if (displayNumber >= 0 && !isFinishing() && frameCallback != null) {
                    if (frameTimeNanos - lastFrameRequestNanos >= FRAME_INTERVAL_NANOS) {
                        lastFrameRequestNanos = frameTimeNanos
                        X11Bridge.requestX11Frame(displayNumber)
                    }
                    choreographer.postFrameCallback(this)
                }
            }
        }
        choreographer.postFrameCallback(frameCallback!!)
        X11Bridge.requestX11Frame(displayNumber)
        scheduleIdleLoop()
        Log.d(TAG, "Started frame and idle loops for display $displayNumber")
    }

    private fun stopFrameAndIdleLoops() {
        frameCallback = null
        idleScheduled = false
    }

    private fun scheduleIdleLoop() {
        if (!idleScheduled || isFinishing() || displayNumber < 0) return
        X11DisplayManager.pluginUiExecutor.execute {
            if (!idleScheduled || isFinishing()) return@execute
            X11Bridge.idlePluginUIs()
            mainHandler.postDelayed({
                if (idleScheduled && !isFinishing() && displayNumber >= 0) scheduleIdleLoop()
            }, PLUGIN_IDLE_INTERVAL_MS)
        }
    }
    companion object {
        const val EXTRA_PLUGIN_ID = "com.vibes.dsp.extra.PLUGIN_ID"
        const val EXTRA_ORIENTATION = "com.vibes.dsp.extra.ORIENTATION"

        fun intent(context: android.content.Context, pluginId: String, orientation: DisplayOrientation? = null) =
            android.content.Intent(context, X11PluginUIActivity::class.java).apply {
                putExtra(EXTRA_PLUGIN_ID, pluginId)
                orientation?.let { putExtra(EXTRA_ORIENTATION, it.persisted) }
            }

        private const val TAG = "X11PluginUIActivity"
        private const val PLUGIN_IDLE_INTERVAL_MS = 17L
        private const val X11_INIT_DELAY_MS = 150L
        private const val FRAME_INTERVAL_NANOS = 16_000_000L
    }

    private var pluginId: String? = null
    private var pathId: Long = -1L
    private var pluginIndex: Int = -1
    private var pluginInstanceId: Long = 0L
    private var uiInstanceId: Long = 0L
    private var displayNumber: Int = -1
    private var attached: Boolean = false
    private var pendingDetach: Boolean = false
    private var frameCallback: Choreographer.FrameCallback? = null
    private val mainHandler = Handler(Looper.getMainLooper())
    private var lastFrameRequestNanos: Long = 0L
    private var idleScheduled: Boolean = false
    private var touchLogCount = 0
    private var geometryObserver: ScreenGeometryObserver? = null

}
