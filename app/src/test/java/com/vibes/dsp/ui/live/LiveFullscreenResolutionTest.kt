package com.vibes.dsp.ui.live

import com.vibes.dsp.ui.rack.RackPlugin
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class LiveFullscreenResolutionTest {
    @Test
    fun resolvesTheRequestedPluginFromTheCurrentlySelectedPath() {
        val requested = RackPlugin(index = 3, name = "Amp", pluginId = "amp", instanceId = 42L)
        val request = LiveFullscreenRequest(
            instanceId = requested.instanceId,
            pathId = 7L,
            width = 1600,
            height = 900,
        )

        assertEquals(
            requested,
            resolveLiveFullscreenPlugin(
                request = request,
                selectedPathId = 7L,
                plugins = listOf(
                    RackPlugin(0, "Delay", "delay", 11L),
                    requested,
                ),
            ),
        )
    }

    @Test
    fun rejectsAStalePathOrMissingInstanceSoFullscreenCanExit() {
        val request = LiveFullscreenRequest(
            instanceId = 42L,
            pathId = 7L,
            width = 1600,
            height = 900,
        )
        val plugins = listOf(RackPlugin(3, "Amp", "amp", 42L))

        assertNull(resolveLiveFullscreenPlugin(request, selectedPathId = 8L, plugins = plugins))
        assertNull(
            resolveLiveFullscreenPlugin(
                request,
                selectedPathId = 7L,
                plugins = listOf(RackPlugin(3, "Amp", "amp", 99L)),
            ),
        )
    }
}
