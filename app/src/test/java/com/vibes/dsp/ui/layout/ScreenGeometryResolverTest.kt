package com.vibes.dsp.ui.layout

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class ScreenGeometryResolverTest {
    @Test
    fun emptySnapshotHasNoSafeAreaOrFeatureObstructions() {
        val geometry = resolveScreenGeometry(
            width = 1080,
            height = 1920,
            systemInsets = PixelInsets(),
            mandatoryGestureInsets = PixelInsets(),
            cutoutInsets = PixelInsets(),
            waterfallInsets = PixelInsets(),
            cutouts = emptyList(),
            roundedCorners = emptyList(),
        )

        assertEquals(1080, geometry.windowWidth)
        assertEquals(1920, geometry.windowHeight)
        assertEquals(PixelInsets(), geometry.safeInsets())
        assertEquals(listOf(EdgeSafeSegment(0, 1080)), geometry.edgeSafeSegments(DisplayEdge.Top, 48, 4))
        assertTrue(geometry.cutouts.isEmpty())
        assertTrue(geometry.roundedCorners.isEmpty())
    }

    @Test
    fun cutoutsOnEveryEdgeRemainIndependentAndClassifiedByWindowBounds() {
        val width = 1080
        val height = 1920
        val cutouts = listOf(
            obstruction(PixelRect(400, -20, 680, 90), width, height),
            obstruction(PixelRect(-20, 700, 100, 1000), width, height),
            obstruction(PixelRect(980, 800, 1100, 1050), width, height),
            obstruction(PixelRect(450, 1840, 650, 1940), width, height),
        )
        val geometry = resolveScreenGeometry(
            width = width,
            height = height,
            systemInsets = PixelInsets(),
            mandatoryGestureInsets = PixelInsets(),
            cutoutInsets = PixelInsets(left = 100, top = 90, right = 100, bottom = 80),
            waterfallInsets = PixelInsets(),
            cutouts = cutouts,
            roundedCorners = emptyList(),
        )

        assertEquals(
            listOf(
                setOf(DisplayEdge.Top),
                setOf(DisplayEdge.Left),
                setOf(DisplayEdge.Right),
                setOf(DisplayEdge.Bottom),
            ),
            geometry.cutouts.map { it.edges },
        )
        assertEquals(PixelInsets(100, 90, 100, 80), geometry.safeInsets())
        assertEquals(
            PixelInsets(left = 0, top = 0, right = 100, bottom = 80),
            geometry.safeInsets(setOf(DisplayEdge.Left, DisplayEdge.Top)),
        )
    }

    @Test
    fun emptyRoundedCornerInputModelsDevicesWithoutRoundedCornerApi() {
        val geometry = resolveScreenGeometry(
            width = 1920,
            height = 1080,
            systemInsets = PixelInsets(left = 7, top = 8, right = 9, bottom = 10),
            mandatoryGestureInsets = PixelInsets(left = 12, top = 13, right = 14, bottom = 15),
            cutoutInsets = PixelInsets(),
            waterfallInsets = PixelInsets(),
            cutouts = emptyList(),
            roundedCorners = emptyList(),
        )

        assertEquals(PixelInsets(), geometry.roundedCornerInsets)
        assertEquals(PixelInsets(12, 13, 14, 15), geometry.safeInsets())
    }

    @Test
    fun roundedCornersUseTopBottomExtentsInPortrait() {
        val corners = listOf(
            RoundedCornerGeometry(0, centerX = 40, centerY = 60, radius = 40),
            RoundedCornerGeometry(1, centerX = 1040, centerY = 85, radius = 40),
            RoundedCornerGeometry(2, centerX = 1035, centerY = 1840, radius = 40),
            RoundedCornerGeometry(3, centerX = 45, centerY = 1865, radius = 40),
        )

        assertEquals(
            PixelInsets(top = 85, bottom = 80),
            ScreenGeometryResolver.roundedInsets(1080, 1920, corners),
        )
    }

    @Test
    fun roundedCornersUseLeftRightExtentsInLandscape() {
        val corners = listOf(
            RoundedCornerGeometry(0, centerX = 70, centerY = 40, radius = 40),
            RoundedCornerGeometry(1, centerX = 95, centerY = 1040, radius = 40),
            RoundedCornerGeometry(2, centerX = 1825, centerY = 1040, radius = 40),
            RoundedCornerGeometry(3, centerX = 1860, centerY = 40, radius = 40),
        )

        assertEquals(
            PixelInsets(left = 95, right = 95),
            ScreenGeometryResolver.roundedInsets(1920, 1080, corners),
        )
    }

    @Test
    fun topSegmentsSubtractCutoutAndPreserveSafeSideInsets() {
        val geometry = resolveScreenGeometry(
            width = 1000,
            height = 600,
            systemInsets = PixelInsets(),
            mandatoryGestureInsets = PixelInsets(left = 20, right = 30),
            cutoutInsets = PixelInsets(),
            waterfallInsets = PixelInsets(),
            cutouts = listOf(
                obstruction(PixelRect(300, 0, 500, 60), 1000, 600),
            ),
            roundedCorners = emptyList(),
        )

        assertEquals(
            listOf(EdgeSafeSegment(20, 290), EdgeSafeSegment(510, 970)),
            geometry.edgeSafeSegments(DisplayEdge.Top, touchDepthPx = 48, gapPx = 10),
        )
    }

    @Test
    fun sideSegmentsCanLeaveSpaceAboveAndBelowSideCutout() {
        val geometry = resolveScreenGeometry(
            width = 1000,
            height = 600,
            systemInsets = PixelInsets(),
            mandatoryGestureInsets = PixelInsets(top = 20, bottom = 30),
            cutoutInsets = PixelInsets(),
            waterfallInsets = PixelInsets(),
            cutouts = listOf(
                obstruction(PixelRect(0, 200, 70, 400), 1000, 600),
            ),
            roundedCorners = emptyList(),
        )

        assertEquals(
            listOf(EdgeSafeSegment(20, 195), EdgeSafeSegment(405, 570)),
            geometry.edgeSafeSegments(DisplayEdge.Left, touchDepthPx = 48, gapPx = 5),
        )
    }

    @Test
    fun safeInsetsTakePerEdgeMaximumAcrossGestureCutoutWaterfallAndRoundedSources() {
        val geometry = resolveScreenGeometry(
            width = 1000,
            height = 600,
            systemInsets = PixelInsets(99, 99, 99, 99),
            mandatoryGestureInsets = PixelInsets(12, 20, 30, 40),
            cutoutInsets = PixelInsets(40, 10, 5, 60),
            waterfallInsets = PixelInsets(50, 25, 35, 10),
            cutouts = emptyList(),
            roundedCorners = listOf(
                RoundedCornerGeometry(0, centerX = 40, centerY = 15, radius = 15),
                RoundedCornerGeometry(1, centerX = 960, centerY = 15, radius = 15),
            ),
        )

        assertEquals(PixelInsets(50, 25, 40, 60), geometry.safeInsets())
    }

    @Test
    fun clampDoesNotApplyASecondScreenOriginShiftToWindowCoordinates() {
        val windowLocalRect = PixelRect(240, 320, 340, 410)

        assertEquals(
            PixelRect(240, 320, 340, 410),
            ScreenGeometryResolver.clamp(windowLocalRect, width = 1000, height = 600),
        )
    }

    private fun obstruction(rect: PixelRect, width: Int, height: Int): DisplayObstruction {
        val clamped = ScreenGeometryResolver.clamp(rect, width, height)
            ?: error("test obstruction must intersect the window")
        return DisplayObstruction(clamped, ScreenGeometryResolver.classify(clamped, width, height))
    }
}
