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

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import android.graphics.Rect
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.provider.OpenableColumns
import android.text.InputType
import android.util.AttributeSet
import android.util.Log
import android.view.DragEvent
import android.view.Gravity
import android.view.InputDevice
import android.view.KeyCharacterMap
import android.view.KeyEvent
import android.view.Menu
import android.view.MotionEvent
import android.view.PointerIcon
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.widget.FrameLayout
import android.widget.PopupMenu
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.layout.layout
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import com.vibes.dsp.engine.JsfxBridge
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File
import java.util.concurrent.atomic.AtomicInteger

private const val INVALID_POINTER_ID = -1
private const val TAG = "JsfxPluginUi"
private const val POLL_INTERVAL_MS = 50L
private const val FALLBACK_WIDTH = 640
private const val FALLBACK_HEIGHT = 360
private const val MAX_MENU_DEPTH = 8
private val minimumInlineHeight = 160.dp
private val maximumInlineHeight = 520.dp

private const val YSFX_MOD_SHIFT = 1
private const val YSFX_MOD_CTRL = 2
private const val YSFX_MOD_ALT = 4
private const val YSFX_MOD_SUPER = 8
private const val YSFX_BUTTON_LEFT = 1
private const val YSFX_BUTTON_MIDDLE = 2
private const val YSFX_BUTTON_RIGHT = 4
private const val YSFX_KEY_F1 = 0xE000
private const val YSFX_KEY_LEFT = YSFX_KEY_F1 + 12
private const val YSFX_KEY_UP = YSFX_KEY_LEFT + 1
private const val YSFX_KEY_RIGHT = YSFX_KEY_LEFT + 2
private const val YSFX_KEY_DOWN = YSFX_KEY_LEFT + 3
private const val YSFX_KEY_PAGE_UP = YSFX_KEY_LEFT + 4
private const val YSFX_KEY_PAGE_DOWN = YSFX_KEY_LEFT + 5
private const val YSFX_KEY_HOME = YSFX_KEY_LEFT + 6
private const val YSFX_KEY_END = YSFX_KEY_LEFT + 7
private const val YSFX_KEY_INSERT = YSFX_KEY_LEFT + 8

private data class PreferredSize(val width: Int, val height: Int)
private data class MenuRequest(val id: Long, val spec: String, val x: Int, val y: Int)

/**
 * A stable Android host for one rack plugin's ysfx surface. The view may be measured to zero while
 * another UI mode is selected, but remains composed and keeps the native plugin instance intact.
 */
@Composable
fun JsfxPluginUi(
    pathId: Long,
    pluginInstanceId: Long,
    pluginName: String,
    isVisible: Boolean,
    isFullscreen: Boolean = false,
    modifier: Modifier = Modifier,
    onPreferredSize: (width: Int, height: Int) -> Unit = { _, _ -> },
) {
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current
    val surfaceView = remember(pathId, pluginInstanceId) {
        JsfxSurfaceView(context, pathId, pluginInstanceId).apply {
            contentDescription = "$pluginName JSFX interface"
        }
    }
    var lifecycleStarted by remember(lifecycleOwner) {
        mutableStateOf(lifecycleOwner.lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED))
    }
    var preferredSize by remember(pathId, pluginInstanceId) {
        mutableStateOf(PreferredSize(FALLBACK_WIDTH, FALLBACK_HEIGHT))
    }

    LaunchedEffect(pathId, pluginInstanceId) {
        val encoded = withContext(Dispatchers.IO) {
            runCatching { JsfxBridge.nativeGetPreferredSize(pathId, pluginInstanceId) }.getOrDefault(0L)
        }
        val width = (encoded ushr 32).toInt().takeIf { it > 0 } ?: FALLBACK_WIDTH
        val height = (encoded and 0xFFFFFFFFL).toInt().takeIf { it > 0 } ?: FALLBACK_HEIGHT
        preferredSize = PreferredSize(width, height)
        onPreferredSize(width, height)
    }

    DisposableEffect(lifecycleOwner, surfaceView) {
        val observer = LifecycleEventObserver { _, event ->
            lifecycleStarted = lifecycleOwner.lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)
            if (event == Lifecycle.Event.ON_STOP) surfaceView.clearDroppedFiles()
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }

    DisposableEffect(surfaceView) {
        onDispose { surfaceView.release() }
    }

    val hostVisible = isVisible && lifecycleStarted
    BoxWithConstraints(
        modifier = modifier.fillMaxWidth(),
        contentAlignment = Alignment.Center,
    ) {
        val aspectRatio = preferredSize.width.toFloat() / preferredSize.height.toFloat()
        val inlineHeight = (maxWidth / aspectRatio).coerceIn(minimumInlineHeight, maximumInlineHeight)
        val availableHeight = maxHeight.takeUnless { it == Dp.Infinity } ?: inlineHeight
        val targetHeight = if (isFullscreen) {
            minOf(availableHeight, maxWidth / aspectRatio)
        } else {
            inlineHeight
        }
        val targetWidth = if (isFullscreen) {
            minOf(maxWidth, targetHeight * aspectRatio)
        } else {
            maxWidth
        }

        AndroidView(
            factory = { surfaceView },
            update = { view ->
                view.contentDescription = "$pluginName JSFX interface"
                view.setHostVisible(hostVisible)
            },
            modifier = Modifier
                .keepMeasuredWhenHidden(hostVisible, targetWidth, targetHeight)
                .alpha(if (hostVisible) 1f else 0f)
                .background(androidx.compose.material3.MaterialTheme.colorScheme.surfaceVariant),
        )
    }
}

private fun Modifier.keepMeasuredWhenHidden(visible: Boolean, width: Dp, height: Dp): Modifier =
    layout { measurable, _ ->
        val measuredWidth = width.roundToPx().coerceAtLeast(1)
        val measuredHeight = height.roundToPx().coerceAtLeast(1)
        val placeable = measurable.measure(Constraints.fixed(measuredWidth, measuredHeight))
        layout(
            width = if (visible) measuredWidth else 0,
            height = if (visible) measuredHeight else 0,
        ) {
            placeable.place(0, 0)
        }
    }

private class JsfxSurfaceView @JvmOverloads constructor(
    context: Context,
    private val pathId: Long,
    private val pluginInstanceId: Long,
    attrs: AttributeSet? = null,
) : SurfaceView(context, attrs), SurfaceHolder.Callback2 {
    private val mainHandler = Handler(Looper.getMainLooper())
    private val ioScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val dropGeneration = AtomicInteger()
    private val dropRoot = File(
        context.cacheDir,
        "jsfx-drops/$pathId-$pluginInstanceId-${System.identityHashCode(this)}",
    )
    private var attachedToNative = false
    private var released = false
    private var hostVisible = false
    private var primaryPointerId = INVALID_POINTER_ID
    private var popupMenu: PopupMenu? = null
    private var popupAnchor: View? = null
    private var popupRequestId = 0L
    private var popupResponded = false
    private var lastCursor = Int.MIN_VALUE
    private var pollScheduled = false

    private val pollRunnable = object : Runnable {
        override fun run() {
            pollScheduled = false
            if (!released && isAttachedToWindow) {
                if (attachedToNative) {
                    pollMenu()
                    if (hostVisible) pollCursor()
                }
                schedulePoll()
            }
        }
    }

    init {
        holder.addCallback(this)
        isFocusable = true
        isFocusableInTouchMode = true
        importantForAccessibility = IMPORTANT_FOR_ACCESSIBILITY_YES
        setOnDragListener(::handleDragEvent)
    }

    fun setHostVisible(visible: Boolean) {
        if (released || hostVisible == visible) return
        hostVisible = visible
        syncWindowState()
        if (!visible) dismissPopup(respond = true)
    }

    fun clearDroppedFiles() {
        dropGeneration.incrementAndGet()
        if (!released) runCatching { JsfxBridge.nativeSetDropFiles(pathId, pluginInstanceId, emptyArray()) }
        ioScope.launch(Dispatchers.IO) { dropRoot.deleteRecursively() }
    }

    fun release() {
        if (released) return
        dismissPopup(respond = true)
        hostVisible = false
        runCatching { JsfxBridge.nativeSetFocus(pathId, pluginInstanceId, false) }
        runCatching { JsfxBridge.nativeSetVisible(pathId, pluginInstanceId, false) }
        clearDroppedFiles()
        released = true
        mainHandler.removeCallbacks(pollRunnable)
        pollScheduled = false
        if (attachedToNative) {
            runCatching { JsfxBridge.nativeDetach(pathId, pluginInstanceId) }
            attachedToNative = false
        }
        holder.removeCallback(this)
        ioScope.cancel()
        dropRoot.deleteRecursively()
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        schedulePoll()
        syncWindowState()
    }

    override fun onDetachedFromWindow() {
        runCatching { JsfxBridge.nativeSetFocus(pathId, pluginInstanceId, false) }
        runCatching { JsfxBridge.nativeSetVisible(pathId, pluginInstanceId, false) }
        dismissPopup(respond = true)
        mainHandler.removeCallbacks(pollRunnable)
        pollScheduled = false
        super.onDetachedFromWindow()
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        if (released || !holder.surface.isValid || width <= 0 || height <= 0) return
        attachedToNative = runCatching {
            JsfxBridge.nativeAttach(pathId, pluginInstanceId, holder.surface, width, height)
        }.getOrElse {
            Log.e(TAG, "Unable to attach JSFX surface", it)
            false
        }
        syncWindowState()
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        if (released || width <= 0 || height <= 0) return
        if (!attachedToNative && holder.surface.isValid) {
            surfaceCreated(holder)
        } else if (attachedToNative) {
            JsfxBridge.nativeResize(pathId, pluginInstanceId, width, height)
        }
    }

    override fun surfaceRedrawNeeded(holder: SurfaceHolder) = Unit

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        if (!attachedToNative) return
        runCatching { JsfxBridge.nativeSetFocus(pathId, pluginInstanceId, false) }
        runCatching { JsfxBridge.nativeSetVisible(pathId, pluginInstanceId, false) }
        runCatching { JsfxBridge.nativeDetach(pathId, pluginInstanceId) }
        attachedToNative = false
    }

    override fun onFocusChanged(gainFocus: Boolean, direction: Int, previouslyFocusedRect: Rect?) {
        super.onFocusChanged(gainFocus, direction, previouslyFocusedRect)
        if (!released) {
            JsfxBridge.nativeSetFocus(pathId, pluginInstanceId, gainFocus && hostVisible && hasWindowFocus())
        }
    }

    override fun onWindowFocusChanged(hasWindowFocus: Boolean) {
        super.onWindowFocusChanged(hasWindowFocus)
        syncWindowState()
    }

    override fun onWindowVisibilityChanged(visibility: Int) {
        super.onWindowVisibilityChanged(visibility)
        syncWindowState()
    }

    private fun syncWindowState() {
        if (released) return
        val visible = attachedToNative && hostVisible && windowVisibility == VISIBLE && isShown
        runCatching { JsfxBridge.nativeSetVisible(pathId, pluginInstanceId, visible) }
        runCatching { JsfxBridge.nativeSetFocus(pathId, pluginInstanceId, visible && hasFocus() && hasWindowFocus()) }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (!hostVisible || released) return false
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                primaryPointerId = event.getPointerId(event.actionIndex)
                requestFocus()
                parent?.requestDisallowInterceptTouchEvent(true)
                sendPointer(event, event.actionIndex, MotionEvent.ACTION_DOWN, touchButtonDown = true)
            }
            MotionEvent.ACTION_MOVE -> {
                val index = event.findPointerIndex(primaryPointerId)
                if (index >= 0) sendPointer(event, index, MotionEvent.ACTION_MOVE, touchButtonDown = true)
            }
            MotionEvent.ACTION_POINTER_UP -> {
                if (event.getPointerId(event.actionIndex) == primaryPointerId) {
                    sendPointer(event, event.actionIndex, MotionEvent.ACTION_UP, touchButtonDown = false)
                    primaryPointerId = INVALID_POINTER_ID
                    parent?.requestDisallowInterceptTouchEvent(false)
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                if (primaryPointerId != INVALID_POINTER_ID) {
                    val index = event.findPointerIndex(primaryPointerId).takeIf { it >= 0 } ?: event.actionIndex
                    sendPointer(event, index, event.actionMasked, touchButtonDown = false)
                    if (event.actionMasked == MotionEvent.ACTION_UP) performClick()
                }
                primaryPointerId = INVALID_POINTER_ID
                parent?.requestDisallowInterceptTouchEvent(false)
            }
        }
        return true
    }

    override fun performClick(): Boolean {
        super.performClick()
        return true
    }

    override fun onHoverEvent(event: MotionEvent): Boolean {
        if (!hostVisible || released) return false
        sendPointer(event, event.actionIndex.coerceAtLeast(0), event.actionMasked, touchButtonDown = false)
        return true
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if (!hostVisible || released || event.source and InputDevice.SOURCE_CLASS_POINTER == 0) {
            return super.onGenericMotionEvent(event)
        }
        val action = event.actionMasked
        if (action == MotionEvent.ACTION_SCROLL || action == MotionEvent.ACTION_HOVER_MOVE ||
            action == MotionEvent.ACTION_BUTTON_PRESS || action == MotionEvent.ACTION_BUTTON_RELEASE
        ) {
            if (action == MotionEvent.ACTION_BUTTON_PRESS) requestFocus()
            sendPointer(event, event.actionIndex.coerceAtLeast(0), action, touchButtonDown = false)
            return true
        }
        return super.onGenericMotionEvent(event)
    }

    private fun sendPointer(event: MotionEvent, index: Int, action: Int, touchButtonDown: Boolean) {
        if (index !in 0 until event.pointerCount) return
        val buttons = mapButtons(event.buttonState) or if (touchButtonDown) YSFX_BUTTON_LEFT else 0
        JsfxBridge.nativePointer(
            pathId,
            pluginInstanceId,
            action,
            event.getPointerId(index),
            event.getX(index),
            event.getY(index),
            buttons,
            if (action == MotionEvent.ACTION_SCROLL) event.getAxisValue(MotionEvent.AXIS_HSCROLL) else 0f,
            if (action == MotionEvent.ACTION_SCROLL) event.getAxisValue(MotionEvent.AXIS_VSCROLL) else 0f,
            mapModifiers(event.metaState),
        )
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if (!hostVisible) return super.onKeyDown(keyCode, event)
        sendKey(event, down = true)
        return true
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        if (!hostVisible) return super.onKeyUp(keyCode, event)
        sendKey(event, down = false)
        return true
    }

    private fun sendKey(event: KeyEvent, down: Boolean) {
        val unicode = event.unicodeChar and KeyCharacterMap.COMBINING_ACCENT_MASK.inv()
        JsfxBridge.nativeKey(
            pathId,
            pluginInstanceId,
            down,
            mapSpecialKey(event.keyCode),
            unicode,
            mapModifiers(event.metaState),
            event.repeatCount,
        )
    }

    override fun onCheckIsTextEditor(): Boolean = true

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI
        return object : BaseInputConnection(this, false) {
            override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
                text?.codePoints()?.forEach { codePoint ->
                    JsfxBridge.nativeKey(pathId, pluginInstanceId, true, 0, codePoint, 0, 0)
                    JsfxBridge.nativeKey(pathId, pluginInstanceId, false, 0, codePoint, 0, 0)
                }
                return true
            }

            override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
                JsfxBridge.nativeKey(pathId, pluginInstanceId, true, 0x08, 0, 0, 0)
                JsfxBridge.nativeKey(pathId, pluginInstanceId, false, 0x08, 0, 0, 0)
                return true
            }
        }
    }

    private fun handleDragEvent(view: View, event: DragEvent): Boolean = when (event.action) {
        DragEvent.ACTION_DRAG_STARTED -> event.clipDescription?.mimeTypeCount?.let { it > 0 } == true
        DragEvent.ACTION_DRAG_ENTERED -> {
            view.requestFocus()
            true
        }
        DragEvent.ACTION_DROP -> {
            copyDropData(event)
            true
        }
        DragEvent.ACTION_DRAG_ENDED, DragEvent.ACTION_DRAG_EXITED, DragEvent.ACTION_DRAG_LOCATION -> true
        else -> false
    }

    private fun copyDropData(event: DragEvent) {
        val clipData = event.clipData ?: return
        val generation = dropGeneration.incrementAndGet()
        val permission = context.findActivity()?.requestDragAndDropPermissions(event)
        ioScope.launch {
            try {
                val paths = withContext(Dispatchers.IO) {
                    val batchDir = File(dropRoot, generation.toString()).apply { mkdirs() }
                    buildList {
                        for (index in 0 until clipData.itemCount) {
                            copyDropItem(
                                clipData.getItemAt(index).uri,
                                clipData.getItemAt(index).text,
                                batchDir,
                                index,
                            )?.let(::add)
                        }
                    }
                }
                if (!released && generation == dropGeneration.get()) {
                    JsfxBridge.nativeSetDropFiles(pathId, pluginInstanceId, paths.toTypedArray())
                    withContext(Dispatchers.IO) {
                        dropRoot.listFiles()
                            ?.filter { it.name != generation.toString() }
                            ?.forEach { it.deleteRecursively() }
                    }
                }
            } finally {
                permission?.release()
            }
        }
    }

    private fun copyDropItem(uri: Uri?, text: CharSequence?, batchDir: File, index: Int): String? {
        val localFile = when {
            uri?.scheme == "file" -> uri.path?.let(::File)
            uri == null -> text?.toString()?.let(::File)
            else -> null
        }?.takeIf(File::isFile)
        if (localFile != null) {
            val destination = uniqueDestination(batchDir, sanitizeFileName(localFile.name))
            return runCatching {
                localFile.inputStream().use { input ->
                    destination.outputStream().use { output -> input.copyTo(output) }
                }
                destination.absolutePath
            }.onFailure { Log.e(TAG, "Unable to copy dropped file", it) }.getOrNull()
        }
        if (uri != null) {
            val displayName = queryDisplayName(uri) ?: "drop-$index"
            val destination = uniqueDestination(batchDir, sanitizeFileName(displayName))
            return runCatching {
                context.contentResolver.openInputStream(uri)?.use { input ->
                    destination.outputStream().use { output -> input.copyTo(output) }
                } ?: return null
                destination.absolutePath
            }.onFailure { Log.e(TAG, "Unable to copy dropped URI", it) }.getOrNull()
        }
        return null
    }

    private fun queryDisplayName(uri: Uri): String? = runCatching {
        context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) cursor.getString(0) else null
        }
    }.getOrNull()

    private fun schedulePoll() {
        if (released || pollScheduled) return
        pollScheduled = true
        mainHandler.postDelayed(pollRunnable, POLL_INTERVAL_MS)
    }

    private fun pollMenu() {
        if (popupMenu != null) return
        val encoded = runCatching { JsfxBridge.nativePollMenu(pathId, pluginInstanceId) }.getOrNull() ?: return
        val request = runCatching {
            JSONObject(encoded).let { json ->
                MenuRequest(
                    id = json.getLong("id"),
                    spec = json.getString("spec"),
                    x = json.optInt("x"),
                    y = json.optInt("y"),
                )
            }
        }.getOrElse {
            Log.e(TAG, "Invalid JSFX menu request", it)
            return
        }
        if (hostVisible) showPopup(request)
        else JsfxBridge.nativeRespondMenu(pathId, pluginInstanceId, request.id, 0)
    }

    private fun showPopup(request: MenuRequest) {
        val entries = parseJsfxMenu(request.spec)
        if (entries.isEmpty()) {
            JsfxBridge.nativeRespondMenu(pathId, pluginInstanceId, request.id, 0)
            return
        }
        val anchor = createPopupAnchor(request.x, request.y)
        val popup = PopupMenu(context, anchor, Gravity.NO_GRAVITY)
        addMenuEntries(popup.menu, entries, depth = 0)
        popupRequestId = request.id
        popupResponded = false
        popupAnchor = anchor.takeUnless { it === this }
        popupMenu = popup
        popup.setOnMenuItemClickListener { item ->
            if (item.itemId <= 0) return@setOnMenuItemClickListener false
            respondPopup(item.itemId)
            true
        }
        popup.setOnDismissListener {
            if (!popupResponded) respondPopup(0)
            removePopupAnchor()
            popupMenu = null
        }
        runCatching { popup.show() }.onFailure { error ->
            Log.e(TAG, "Unable to show JSFX popup menu", error)
            respondPopup(0)
            popupMenu = null
            removePopupAnchor()
        }
    }

    private fun addMenuEntries(menu: Menu, entries: List<JsfxMenuEntry>, depth: Int) {
        if (depth >= MAX_MENU_DEPTH) return
        entries.forEachIndexed { order, entry ->
            when (entry) {
                JsfxMenuEntry.Separator -> menu.add(Menu.NONE, View.NO_ID, order, "").isEnabled = false
                is JsfxMenuEntry.Item -> {
                    if (entry.children.isEmpty()) {
                        menu.add(Menu.NONE, entry.id, order, entry.label).apply {
                            isEnabled = entry.enabled
                            isCheckable = entry.checked
                            isChecked = entry.checked
                        }
                    } else {
                        val submenu = menu.addSubMenu(Menu.NONE, View.NO_ID, order, entry.label)
                        submenu.item.isEnabled = entry.enabled
                        submenu.item.isCheckable = entry.checked
                        submenu.item.isChecked = entry.checked
                        addMenuEntries(submenu, entry.children, depth + 1)
                    }
                }
            }
        }
    }

    private fun createPopupAnchor(x: Int, y: Int): View {
        val content = context.findActivity()?.findViewById<ViewGroup>(android.R.id.content) ?: return this
        val viewPosition = IntArray(2)
        val contentPosition = IntArray(2)
        getLocationInWindow(viewPosition)
        content.getLocationInWindow(contentPosition)
        return View(context).also { anchor ->
            content.addView(
                anchor,
                FrameLayout.LayoutParams(1, 1).apply {
                    leftMargin = viewPosition[0] - contentPosition[0] + x.coerceIn(0, width)
                    topMargin = viewPosition[1] - contentPosition[1] + y.coerceIn(0, height)
                },
            )
        }
    }

    private fun respondPopup(itemId: Int) {
        if (popupResponded) return
        popupResponded = true
        JsfxBridge.nativeRespondMenu(pathId, pluginInstanceId, popupRequestId, itemId)
    }

    private fun dismissPopup(respond: Boolean) {
        if (respond && popupMenu != null) respondPopup(0)
        popupMenu?.dismiss()
        popupMenu = null
        removePopupAnchor()
    }

    private fun removePopupAnchor() {
        popupAnchor?.let { (it.parent as? ViewGroup)?.removeView(it) }
        popupAnchor = null
    }

    private fun pollCursor() {
        val cursor = runCatching { JsfxBridge.nativeGetCursor(pathId, pluginInstanceId) }.getOrDefault(0)
        if (cursor == lastCursor) return
        lastCursor = cursor
        pointerIcon = PointerIcon.getSystemIcon(context, pointerIconType(cursor))
    }
}

private fun mapButtons(buttonState: Int): Int {
    var buttons = 0
    if (buttonState and MotionEvent.BUTTON_PRIMARY != 0) buttons = buttons or YSFX_BUTTON_LEFT
    if (buttonState and MotionEvent.BUTTON_TERTIARY != 0) buttons = buttons or YSFX_BUTTON_MIDDLE
    if (buttonState and MotionEvent.BUTTON_SECONDARY != 0) buttons = buttons or YSFX_BUTTON_RIGHT
    return buttons
}

private fun mapModifiers(metaState: Int): Int {
    var modifiers = 0
    if (metaState and KeyEvent.META_SHIFT_ON != 0) modifiers = modifiers or YSFX_MOD_SHIFT
    if (metaState and KeyEvent.META_CTRL_ON != 0) modifiers = modifiers or YSFX_MOD_CTRL
    if (metaState and KeyEvent.META_ALT_ON != 0) modifiers = modifiers or YSFX_MOD_ALT
    if (metaState and KeyEvent.META_META_ON != 0) modifiers = modifiers or YSFX_MOD_SUPER
    return modifiers
}

private fun mapSpecialKey(androidKeyCode: Int): Int = when (androidKeyCode) {
    KeyEvent.KEYCODE_DEL -> 0x08
    KeyEvent.KEYCODE_ESCAPE -> 0x1B
    KeyEvent.KEYCODE_FORWARD_DEL -> 0x7F
    in KeyEvent.KEYCODE_F1..KeyEvent.KEYCODE_F12 -> YSFX_KEY_F1 + androidKeyCode - KeyEvent.KEYCODE_F1
    KeyEvent.KEYCODE_DPAD_LEFT -> YSFX_KEY_LEFT
    KeyEvent.KEYCODE_DPAD_UP -> YSFX_KEY_UP
    KeyEvent.KEYCODE_DPAD_RIGHT -> YSFX_KEY_RIGHT
    KeyEvent.KEYCODE_DPAD_DOWN -> YSFX_KEY_DOWN
    KeyEvent.KEYCODE_PAGE_UP -> YSFX_KEY_PAGE_UP
    KeyEvent.KEYCODE_PAGE_DOWN -> YSFX_KEY_PAGE_DOWN
    KeyEvent.KEYCODE_MOVE_HOME -> YSFX_KEY_HOME
    KeyEvent.KEYCODE_MOVE_END -> YSFX_KEY_END
    KeyEvent.KEYCODE_INSERT -> YSFX_KEY_INSERT
    else -> 0
}

private fun pointerIconType(cursor: Int): Int = when (cursor) {
    32513 -> PointerIcon.TYPE_TEXT
    32514, 32650 -> PointerIcon.TYPE_WAIT
    32515 -> PointerIcon.TYPE_CROSSHAIR
    32640, 32646 -> PointerIcon.TYPE_ALL_SCROLL
    32642 -> PointerIcon.TYPE_TOP_LEFT_DIAGONAL_DOUBLE_ARROW
    32643 -> PointerIcon.TYPE_TOP_RIGHT_DIAGONAL_DOUBLE_ARROW
    32644 -> PointerIcon.TYPE_HORIZONTAL_DOUBLE_ARROW
    32645 -> PointerIcon.TYPE_VERTICAL_DOUBLE_ARROW
    32648 -> PointerIcon.TYPE_NO_DROP
    32649 -> PointerIcon.TYPE_HAND
    32651 -> PointerIcon.TYPE_HELP
    else -> PointerIcon.TYPE_ARROW
}

private fun Context.findActivity(): Activity? {
    var current = this
    while (current is ContextWrapper) {
        if (current is Activity) return current
        current = current.baseContext
    }
    return null
}

private fun sanitizeFileName(name: String): String =
    name.substringAfterLast('/').substringAfterLast('\\')
        .replace(Regex("[^A-Za-z0-9._ -]"), "_")
        .take(160)
        .ifBlank { "drop" }

private fun uniqueDestination(directory: File, fileName: String): File {
    var destination = File(directory, fileName)
    var suffix = 2
    val base = destination.nameWithoutExtension
    val extension = destination.extension.takeIf { it.isNotEmpty() }?.let { ".$it" }.orEmpty()
    while (destination.exists()) destination = File(directory, "$base-${suffix++}$extension")
    return destination
}
