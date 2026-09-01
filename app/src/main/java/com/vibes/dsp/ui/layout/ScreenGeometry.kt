/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of NNAGA.
 */
package com.vibes.dsp.ui.layout

import android.content.Context
import android.graphics.Rect
import android.os.Build
import android.view.Display
import android.view.RoundedCorner
import android.view.View
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.composed
import androidx.core.view.OnApplyWindowInsetsListener
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalView
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import com.google.gson.Gson
import com.google.gson.annotations.SerializedName
import kotlin.math.max

data class PixelInsets(
    val left: Int = 0,
    val top: Int = 0,
    val right: Int = 0,
    val bottom: Int = 0,
)

data class PixelRect(val left: Int, val top: Int, val right: Int, val bottom: Int) {
    val width: Int get() = (right - left).coerceAtLeast(0)
    val height: Int get() = (bottom - top).coerceAtLeast(0)
}

enum class DisplayEdge { Top, Right, Bottom, Left }

data class DisplayObstruction(val bounds: PixelRect, val edges: Set<DisplayEdge>)

data class RoundedCornerGeometry(val position: Int, val centerX: Int, val centerY: Int, val radius: Int)

data class EdgeSafeSegment(val startPx: Int, val endPx: Int) {
    val lengthPx: Int get() = endPx - startPx
}

data class ScreenGeometry(
    val windowWidth: Int,
    val windowHeight: Int,
    val systemInsets: PixelInsets = PixelInsets(),
    val mandatoryGestureInsets: PixelInsets = PixelInsets(),
    val cutoutInsets: PixelInsets = PixelInsets(),
    val waterfallInsets: PixelInsets = PixelInsets(),
    val roundedCornerInsets: PixelInsets = PixelInsets(),
    val cutouts: List<DisplayObstruction> = emptyList(),
    val roundedCorners: List<RoundedCornerGeometry> = emptyList(),
    val authoritative: Boolean = false,
) {
    fun safeInsets(excludedCutoutEdges: Set<DisplayEdge> = emptySet()): PixelInsets {
        val includedCutouts = PixelInsets(
            left = cutoutInsets.left.takeUnless { DisplayEdge.Left in excludedCutoutEdges } ?: 0,
            top = cutoutInsets.top.takeUnless { DisplayEdge.Top in excludedCutoutEdges } ?: 0,
            right = cutoutInsets.right.takeUnless { DisplayEdge.Right in excludedCutoutEdges } ?: 0,
            bottom = cutoutInsets.bottom.takeUnless { DisplayEdge.Bottom in excludedCutoutEdges } ?: 0,
        )
        return PixelInsets(
            left = maxOf(mandatoryGestureInsets.left, includedCutouts.left, waterfallInsets.left, roundedCornerInsets.left),
            top = maxOf(mandatoryGestureInsets.top, includedCutouts.top, waterfallInsets.top, roundedCornerInsets.top),
            right = maxOf(mandatoryGestureInsets.right, includedCutouts.right, waterfallInsets.right, roundedCornerInsets.right),
            bottom = maxOf(mandatoryGestureInsets.bottom, includedCutouts.bottom, waterfallInsets.bottom, roundedCornerInsets.bottom),
        )
    }

    /** Free [startPx, endPx) intervals along an edge for a control touching that edge. */
    fun edgeSafeSegments(edge: DisplayEdge, touchDepthPx: Int, gapPx: Int): List<EdgeSafeSegment> {
        val horizontal = edge == DisplayEdge.Top || edge == DisplayEdge.Bottom
        val length = if (horizontal) windowWidth else windowHeight
        if (length <= 0) return emptyList()
        val safe = safeInsets()
        val start = (if (horizontal) safe.left else safe.top).coerceIn(0, length)
        val end = (length - if (horizontal) safe.right else safe.bottom).coerceIn(start, length)
        val depth = touchDepthPx.coerceAtLeast(0)
        val gap = gapPx.coerceAtLeast(0)
        val blocked = buildList {
            cutouts.filter { edge in it.edges && intersectsEdgeBand(it.bounds, edge, depth) }.forEach {
                add(it.bounds.along(edge, windowWidth, windowHeight, gap))
            }
            roundedCorners.map { it.conservativeBounds() }
                .filter { intersectsEdgeBand(it, edge, depth) }
                .forEach { add(it.along(edge, windowWidth, windowHeight, gap)) }
        }.mapNotNull { segment ->
            val clippedStart = segment.startPx.coerceIn(start, end)
            val clippedEnd = segment.endPx.coerceIn(start, end)
            EdgeSafeSegment(clippedStart, clippedEnd).takeIf { it.lengthPx > 0 }
        }.sortedBy(EdgeSafeSegment::startPx)

        val result = mutableListOf<EdgeSafeSegment>()
        var cursor = start
        blocked.forEach { blockedSegment ->
            if (blockedSegment.startPx > cursor) result += EdgeSafeSegment(cursor, blockedSegment.startPx)
            cursor = max(cursor, blockedSegment.endPx)
        }
        if (cursor < end) result += EdgeSafeSegment(cursor, end)
        return result
    }

    private fun PixelRect.along(edge: DisplayEdge, width: Int, height: Int, gap: Int): EdgeSafeSegment = when (edge) {
        DisplayEdge.Top, DisplayEdge.Bottom -> EdgeSafeSegment(left - gap, right + gap)
        DisplayEdge.Left, DisplayEdge.Right -> EdgeSafeSegment(top - gap, bottom + gap)
    }

    private fun intersectsEdgeBand(bounds: PixelRect, edge: DisplayEdge, depth: Int): Boolean = when (edge) {
        DisplayEdge.Top -> bounds.top < depth
        DisplayEdge.Right -> windowWidth - bounds.right < depth
        DisplayEdge.Bottom -> windowHeight - bounds.bottom < depth
        DisplayEdge.Left -> bounds.left < depth
    }

    private fun RoundedCornerGeometry.conservativeBounds(): PixelRect = PixelRect(
        centerX - radius,
        centerY - radius,
        centerX + radius,
        centerY + radius,
    )
}

object ScreenGeometryResolver {
    fun clamp(rect: Rect, width: Int, height: Int): PixelRect? = clamp(
        PixelRect(rect.left, rect.top, rect.right, rect.bottom),
        width,
        height,
    )

    fun clamp(rect: PixelRect, width: Int, height: Int): PixelRect? = PixelRect(
        rect.left.coerceIn(0, width),
        rect.top.coerceIn(0, height),
        rect.right.coerceIn(0, width),
        rect.bottom.coerceIn(0, height),
    ).takeIf { it.width > 0 && it.height > 0 }

    fun classify(rect: PixelRect, width: Int, height: Int): Set<DisplayEdge> = buildSet {
        if (rect.top == 0) add(DisplayEdge.Top)
        if (rect.right == width) add(DisplayEdge.Right)
        if (rect.bottom == height) add(DisplayEdge.Bottom)
        if (rect.left == 0) add(DisplayEdge.Left)
    }

    fun roundedInsets(width: Int, height: Int, corners: List<RoundedCornerGeometry>): PixelInsets {
        if (corners.isEmpty()) return PixelInsets()
        return if (width >= height) {
            PixelInsets(
                left = corners.filter { it.centerX <= width / 2 }.maxOfOrNull { it.centerX } ?: 0,
                right = corners.filter { it.centerX > width / 2 }.maxOfOrNull { width - it.centerX } ?: 0,
            )
        } else {
            PixelInsets(
                top = corners.filter { it.centerY <= height / 2 }.maxOfOrNull { it.centerY } ?: 0,
                bottom = corners.filter { it.centerY > height / 2 }.maxOfOrNull { height - it.centerY } ?: 0,
            )
        }
    }
}

fun resolveScreenGeometry(
    width: Int,
    height: Int,
    systemInsets: PixelInsets,
    mandatoryGestureInsets: PixelInsets,
    cutoutInsets: PixelInsets,
    waterfallInsets: PixelInsets,
    cutouts: List<DisplayObstruction>,
    roundedCorners: List<RoundedCornerGeometry>,
    authoritative: Boolean = true,
): ScreenGeometry = ScreenGeometry(
    windowWidth = width,
    windowHeight = height,
    systemInsets = systemInsets,
    mandatoryGestureInsets = mandatoryGestureInsets,
    cutoutInsets = cutoutInsets,
    waterfallInsets = waterfallInsets,
    roundedCornerInsets = ScreenGeometryResolver.roundedInsets(width, height, roundedCorners),
    cutouts = cutouts,
    roundedCorners = roundedCorners,
    authoritative = authoritative,
)

enum class GeometryCachePolicy { PersistentDefaultDisplay, NoCache }

class ScreenGeometryStore(private val context: Context, private val policy: GeometryCachePolicy) {
    private data class Snapshot(
        @SerializedName("schemaVersion") val schemaVersion: Int,
        val fingerprint: String,
        val geometry: ScreenGeometry,
    )

    private val preferences get() = context.applicationContext.getSharedPreferences("screen_geometry", Context.MODE_PRIVATE)
    private val gson = Gson()

    fun key(displayId: Int, rotation: Int, width: Int, height: Int, densityDpi: Int): String =
        "snapshot:$displayId:$rotation:${width}x$height:$densityDpi"

    fun read(displayId: Int, key: String): ScreenGeometry? {
        if (!canPersist(displayId)) return null
        return preferences.getString(key, null)
            ?.let { runCatching { gson.fromJson(it, Snapshot::class.java) }.getOrNull() }
            ?.takeIf { it.schemaVersion == CACHE_SCHEMA_VERSION && it.fingerprint == Build.FINGERPRINT }
            ?.geometry
            ?.copy(authoritative = false)
    }

    fun write(displayId: Int, key: String, geometry: ScreenGeometry) {
        if (!canPersist(displayId) || !geometry.authoritative) return
        preferences.edit().putString(
            key,
            gson.toJson(Snapshot(CACHE_SCHEMA_VERSION, Build.FINGERPRINT, geometry)),
        ).apply()
    }

    fun canPersist(displayId: Int): Boolean =
        policy == GeometryCachePolicy.PersistentDefaultDisplay && displayId == Display.DEFAULT_DISPLAY

    private companion object {
        const val CACHE_SCHEMA_VERSION = 1
    }
}

class ScreenGeometryObserver(
    private val view: View,
    private val store: ScreenGeometryStore,
    private val onGeometry: (ScreenGeometry) -> Unit,
) {
    private var cacheKey: String? = null
    private val layoutListener = View.OnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> dispatch() }
    private val insetsListener = OnApplyWindowInsetsListener { _, insets ->
        dispatch(insets)
        insets
    }

    fun start() {
        ViewCompat.setOnApplyWindowInsetsListener(view, insetsListener)
        view.addOnLayoutChangeListener(layoutListener)
        view.requestApplyInsets()
        view.post { dispatch() }
    }

    fun stop() {
        ViewCompat.setOnApplyWindowInsetsListener(view, null)
        view.removeOnLayoutChangeListener(layoutListener)
    }

    private fun dispatch(insets: WindowInsetsCompat? = ViewCompat.getRootWindowInsets(view)) {
        val width = view.width
        val height = view.height
        if (width <= 0 || height <= 0) return
        val display = view.display
        val displayId = display?.displayId ?: Display.DEFAULT_DISPLAY
        val key = store.key(
            displayId,
            display?.rotation ?: 0,
            width,
            height,
            view.resources.displayMetrics.densityDpi,
        )
        if (cacheKey != key) {
            cacheKey = key
            store.read(displayId, key)?.let(onGeometry)
        }
        val source = insets ?: return
        val system = source.getInsets(WindowInsetsCompat.Type.systemBars())
        val gestures = source.getInsets(WindowInsetsCompat.Type.mandatorySystemGestures())
        val cutout = source.displayCutout
        val cutouts = cutout?.boundingRects.orEmpty()
            .mapNotNull { ScreenGeometryResolver.clamp(it, width, height) }
            .map { DisplayObstruction(it, ScreenGeometryResolver.classify(it, width, height)) }
        val corners = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            listOf(
                RoundedCorner.POSITION_TOP_LEFT,
                RoundedCorner.POSITION_TOP_RIGHT,
                RoundedCorner.POSITION_BOTTOM_RIGHT,
                RoundedCorner.POSITION_BOTTOM_LEFT,
            ).mapNotNull { position ->
                view.rootWindowInsets?.getRoundedCorner(position)?.let { corner ->
                    RoundedCornerGeometry(
                        position = position,
                        centerX = corner.center.x.coerceIn(0, width),
                        centerY = corner.center.y.coerceIn(0, height),
                        radius = corner.radius.coerceAtLeast(0),
                    )
                }
            }
        } else {
            emptyList()
        }
        val geometry = resolveScreenGeometry(
            width = width,
            height = height,
            systemInsets = PixelInsets(system.left, system.top, system.right, system.bottom),
            mandatoryGestureInsets = PixelInsets(gestures.left, gestures.top, gestures.right, gestures.bottom),
            cutoutInsets = PixelInsets(
                cutout?.safeInsetLeft ?: 0,
                cutout?.safeInsetTop ?: 0,
                cutout?.safeInsetRight ?: 0,
                cutout?.safeInsetBottom ?: 0,
            ),
            waterfallInsets = PixelInsets(
                cutout?.waterfallInsets?.left ?: 0,
                cutout?.waterfallInsets?.top ?: 0,
                cutout?.waterfallInsets?.right ?: 0,
                cutout?.waterfallInsets?.bottom ?: 0,
            ),
            cutouts = cutouts,
            roundedCorners = corners,
        )
        store.write(displayId, key, geometry)
        onGeometry(geometry)
    }
}

val LocalScreenGeometry = compositionLocalOf { ScreenGeometry(0, 0) }

@Composable
fun NnagaScreenGeometryProvider(
    policy: GeometryCachePolicy = GeometryCachePolicy.PersistentDefaultDisplay,
    content: @Composable () -> Unit,
) {
    val context = LocalContext.current
    val view = LocalView.current
    val store = remember(context, policy) { ScreenGeometryStore(context, policy) }
    val displayId = view.display?.displayId ?: Display.DEFAULT_DISPLAY
    var geometry by remember(view, policy) { mutableStateOf(ScreenGeometry(view.width, view.height)) }
    DisposableEffect(view, store, displayId) {
        val observer = ScreenGeometryObserver(view, store) { geometry = it }
        observer.start()
        onDispose(observer::stop)
    }
    CompositionLocalProvider(LocalScreenGeometry provides geometry) {
        Box {
            if (store.canPersist(displayId) || geometry.authoritative) content()
        }
    }
}

fun Modifier.screenSafePadding(excludedCutoutEdges: Set<DisplayEdge> = emptySet()): Modifier = composed {
    val insets = LocalScreenGeometry.current.safeInsets(excludedCutoutEdges)
    with(LocalDensity.current) {
        padding(
            start = insets.left.toDp(),
            top = insets.top.toDp(),
            end = insets.right.toDp(),
            bottom = insets.bottom.toDp(),
        )
    }
}
