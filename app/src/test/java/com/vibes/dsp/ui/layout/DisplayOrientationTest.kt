package com.vibes.dsp.ui.layout

import android.content.pm.ActivityInfo
import org.junit.Assert.assertEquals
import org.junit.Test

class DisplayOrientationTest {
    @Test
    fun orientationsExposeStablePersistedValuesAndPlatformRequests() {
        assertEquals("portrait", DisplayOrientation.Portrait.persisted)
        assertEquals(ActivityInfo.SCREEN_ORIENTATION_PORTRAIT, DisplayOrientation.Portrait.requestedOrientation)
        assertEquals("landscape", DisplayOrientation.Landscape.persisted)
        assertEquals(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE, DisplayOrientation.Landscape.requestedOrientation)
        assertEquals("reverse_landscape", DisplayOrientation.ReverseLandscape.persisted)
        assertEquals(
            ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE,
            DisplayOrientation.ReverseLandscape.requestedOrientation,
        )
    }

    @Test
    fun persistedValuesRoundTripToTheirOrientation() {
        assertEquals(DisplayOrientation.Portrait, DisplayOrientation.fromPersisted("portrait"))
        assertEquals(DisplayOrientation.Landscape, DisplayOrientation.fromPersisted("landscape"))
        assertEquals(DisplayOrientation.ReverseLandscape, DisplayOrientation.fromPersisted("reverse_landscape"))
    }

    @Test
    fun nullAndUnknownPersistedValuesFallBackToPortrait() {
        assertEquals(DisplayOrientation.Portrait, DisplayOrientation.fromPersisted(null))
        assertEquals(DisplayOrientation.Portrait, DisplayOrientation.fromPersisted("sideways"))
        assertEquals(DisplayOrientation.Portrait, DisplayOrientation.fromPersisted(""))
    }
}
