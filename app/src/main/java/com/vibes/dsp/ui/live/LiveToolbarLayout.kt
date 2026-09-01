package com.vibes.dsp.ui.live

import com.vibes.dsp.ui.layout.DisplayEdge
import com.vibes.dsp.ui.layout.ScreenGeometry
import kotlin.math.max

sealed interface LiveToolbarLayout {
    data object Top : LiveToolbarLayout
    data class Vertical(
        val edge: DisplayEdge,
        val railWidthPx: Int,
        val buttonTopOffsetsPx: List<Int>,
    ) : LiveToolbarLayout
}

fun resolveLiveToolbarLayout(
    geometry: ScreenGeometry,
    useVerticalStrip: Boolean,
    buttonCount: Int,
    buttonSizePx: Int,
    gapPx: Int,
): LiveToolbarLayout {
    if (!useVerticalStrip || geometry.windowWidth <= geometry.windowHeight || buttonCount <= 0 || buttonSizePx <= 0) {
        return LiveToolbarLayout.Top
    }
    val topCutout = geometry.cutouts.any { DisplayEdge.Top in it.edges } ||
        geometry.cutoutInsets.top > 0
    val sideEdges = buildSet {
        geometry.cutouts
            .flatMapTo(this) { obstruction ->
                obstruction.edges.filter { it == DisplayEdge.Left || it == DisplayEdge.Right }
            }
        if (geometry.cutoutInsets.left > 0) add(DisplayEdge.Left)
        if (geometry.cutoutInsets.right > 0) add(DisplayEdge.Right)
    }
    if (topCutout || sideEdges.size != 1) return LiveToolbarLayout.Top
    val edge = sideEdges.single()
    val railWidth = max(
        if (edge == DisplayEdge.Left) geometry.cutoutInsets.left else geometry.cutoutInsets.right,
        48,
    )
    val required = buttonCount * buttonSizePx + (buttonCount - 1).coerceAtLeast(0) * gapPx.coerceAtLeast(0)
    val segments = geometry.edgeSafeSegments(edge, buttonSizePx, gapPx.coerceAtLeast(0))
    segments.firstOrNull { it.lengthPx >= required }?.let { segment ->
        return LiveToolbarLayout.Vertical(
            edge = edge,
            railWidthPx = railWidth,
            buttonTopOffsetsPx = List(buttonCount) { index ->
                segment.startPx + index * (buttonSizePx + gapPx.coerceAtLeast(0))
            },
        )
    }
    val gap = gapPx.coerceAtLeast(0)
    var remaining = buttonCount
    val offsets = mutableListOf<Int>()
    segments.forEach { segment ->
        var offset = segment.startPx
        while (remaining > 0 && offset + buttonSizePx <= segment.endPx) {
            offsets += offset
            remaining--
            offset += buttonSizePx + gap
        }
    }
    if (remaining != 0) return LiveToolbarLayout.Top
    return LiveToolbarLayout.Vertical(
        edge = edge,
        railWidthPx = railWidth,
        buttonTopOffsetsPx = offsets,
    )
}
