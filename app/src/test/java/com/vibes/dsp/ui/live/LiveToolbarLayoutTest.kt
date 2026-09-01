package com.vibes.dsp.ui.live

import com.vibes.dsp.ui.layout.DisplayEdge
import com.vibes.dsp.ui.layout.DisplayObstruction
import com.vibes.dsp.ui.layout.PixelInsets
import com.vibes.dsp.ui.layout.PixelRect
import com.vibes.dsp.ui.layout.ScreenGeometry
import com.vibes.dsp.ui.layout.ScreenGeometryResolver
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class LiveToolbarLayoutTest {
    @Test
    fun disabledSwitchKeepsToolbarAtTheTop() {
        val geometry = landscapeWithCutout(DisplayEdge.Left, cutoutInset = 120)

        assertEquals(
            LiveToolbarLayout.Top,
            resolveLiveToolbarLayout(geometry, useVerticalStrip = false, buttonCount = 6, buttonSizePx = 48, gapPx = 4),
        )
    }

    @Test
    fun eligibleLeftRailUsesCutoutWidthAndPlacesAllButtonsInSafeSegment() {
        val geometry = landscapeWithCutout(DisplayEdge.Left, cutoutInset = 120)

        val layout = resolveLiveToolbarLayout(
            geometry = geometry,
            useVerticalStrip = true,
            buttonCount = 6,
            buttonSizePx = 48,
            gapPx = 4,
        )

        assertTrue(layout is LiveToolbarLayout.Vertical)
        layout as LiveToolbarLayout.Vertical
        assertEquals(DisplayEdge.Left, layout.edge)
        assertEquals(120, layout.railWidthPx)
        assertEquals(listOf(304, 356, 408, 460, 512, 564), layout.buttonTopOffsetsPx)
    }

    @Test
    fun eligibleRightRailHasMinimumTouchRailWidth() {
        val geometry = landscapeWithCutout(DisplayEdge.Right, cutoutInset = 24)

        val layout = resolveLiveToolbarLayout(
            geometry = geometry,
            useVerticalStrip = true,
            buttonCount = 6,
            buttonSizePx = 48,
            gapPx = 4,
        )

        assertTrue(layout is LiveToolbarLayout.Vertical)
        layout as LiveToolbarLayout.Vertical
        assertEquals(DisplayEdge.Right, layout.edge)
        assertEquals(48, layout.railWidthPx)
        assertEquals(listOf(304, 356, 408, 460, 512, 564), layout.buttonTopOffsetsPx)
    }

    @Test
    fun sixButtonsMayContinueInTheSafeSegmentBelowASideObstruction() {
        val geometry = ScreenGeometry(
            windowWidth = 1920,
            windowHeight = 900,
            cutoutInsets = PixelInsets(left = 80),
            cutouts = listOf(
                obstruction(PixelRect(0, 300, 80, 600), width = 1920, height = 900),
            ),
            authoritative = true,
        )

        val layout = resolveLiveToolbarLayout(
            geometry = geometry,
            useVerticalStrip = true,
            buttonCount = 6,
            buttonSizePx = 48,
            gapPx = 4,
        )

        assertTrue(layout is LiveToolbarLayout.Vertical)
        layout as LiveToolbarLayout.Vertical
        assertEquals(DisplayEdge.Left, layout.edge)
        assertEquals(listOf(0, 52, 104, 156, 208, 604), layout.buttonTopOffsetsPx)
    }

    @Test
    fun railFallsBackWhenNoSingleSafeSegmentFitsAllButtons() {
        val geometry = ScreenGeometry(
            windowWidth = 1920,
            windowHeight = 500,
            cutoutInsets = PixelInsets(left = 80),
            cutouts = listOf(
                obstruction(PixelRect(0, 100, 80, 300), width = 1920, height = 500),
            ),
            authoritative = true,
        )

        assertEquals(
            LiveToolbarLayout.Top,
            resolveLiveToolbarLayout(geometry, useVerticalStrip = true, buttonCount = 6, buttonSizePx = 48, gapPx = 4),
        )
    }

    @Test
    fun topCutoutDisablesVerticalStripEvenWhenItAlsoTouchesASide() {
        val geometry = ScreenGeometry(
            windowWidth = 1920,
            windowHeight = 900,
            cutoutInsets = PixelInsets(left = 80, top = 100),
            cutouts = listOf(
                obstruction(PixelRect(0, 0, 80, 100), width = 1920, height = 900),
            ),
            authoritative = true,
        )

        assertEquals(
            LiveToolbarLayout.Top,
            resolveLiveToolbarLayout(geometry, useVerticalStrip = true, buttonCount = 6, buttonSizePx = 48, gapPx = 4),
        )
    }

    @Test
    fun cutoutsOnBothSidesDisableVerticalStrip() {
        val geometry = ScreenGeometry(
            windowWidth = 1920,
            windowHeight = 900,
            cutoutInsets = PixelInsets(left = 80, right = 80),
            cutouts = listOf(
                obstruction(PixelRect(0, 100, 80, 250), width = 1920, height = 900),
                obstruction(PixelRect(1840, 100, 1920, 250), width = 1920, height = 900),
            ),
            authoritative = true,
        )

        assertEquals(
            LiveToolbarLayout.Top,
            resolveLiveToolbarLayout(geometry, useVerticalStrip = true, buttonCount = 6, buttonSizePx = 48, gapPx = 4),
        )
    }

    @Test
    fun squareOrNoCutoutLandscapeUsesTopToolbar() {
        val square = ScreenGeometry(windowWidth = 1000, windowHeight = 1000, authoritative = true)
        val landscapeWithoutCutout = ScreenGeometry(windowWidth = 1920, windowHeight = 900, authoritative = true)

        assertEquals(
            LiveToolbarLayout.Top,
            resolveLiveToolbarLayout(square, useVerticalStrip = true, buttonCount = 6, buttonSizePx = 48, gapPx = 4),
        )
        assertEquals(
            LiveToolbarLayout.Top,
            resolveLiveToolbarLayout(
                landscapeWithoutCutout,
                useVerticalStrip = true,
                buttonCount = 6,
                buttonSizePx = 48,
                gapPx = 4,
            ),
        )
    }

    @Test
    fun sideCutoutInsetEnablesVerticalRailWhenBoundingRectsAreEmpty() {
        val geometry = ScreenGeometry(
            windowWidth = 1920,
            windowHeight = 900,
            cutoutInsets = PixelInsets(left = 80),
            cutouts = emptyList(),
            authoritative = true,
        )

        val layout = resolveLiveToolbarLayout(
            geometry = geometry,
            useVerticalStrip = true,
            buttonCount = 6,
            buttonSizePx = 48,
            gapPx = 4,
        )

        assertEquals(
            LiveToolbarLayout.Vertical(
                edge = DisplayEdge.Left,
                railWidthPx = 80,
                buttonTopOffsetsPx = listOf(0, 52, 104, 156, 208, 260),
            ),
            layout,
        )
    }

    private fun landscapeWithCutout(edge: DisplayEdge, cutoutInset: Int): ScreenGeometry {
        val bounds = when (edge) {
            DisplayEdge.Left -> PixelRect(0, 120, cutoutInset, 300)
            DisplayEdge.Right -> PixelRect(1920 - cutoutInset, 120, 1920, 300)
            else -> error("toolbar test only uses side cutouts")
        }
        return ScreenGeometry(
            windowWidth = 1920,
            windowHeight = 900,
            cutoutInsets = when (edge) {
                DisplayEdge.Left -> PixelInsets(left = cutoutInset)
                DisplayEdge.Right -> PixelInsets(right = cutoutInset)
                else -> PixelInsets()
            },
            cutouts = listOf(obstruction(bounds, width = 1920, height = 900)),
            authoritative = true,
        )
    }

    private fun obstruction(rect: PixelRect, width: Int, height: Int): DisplayObstruction {
        val clamped = ScreenGeometryResolver.clamp(rect, width, height)
            ?: error("test obstruction must intersect the window")
        return DisplayObstruction(clamped, ScreenGeometryResolver.classify(clamped, width, height))
    }
}
