package com.vibes.dsp.ui.modgui

import com.vibes.dsp.engine.PluginInfo
import com.vibes.dsp.engine.PortInfo
import org.junit.Assert.assertEquals
import org.junit.Test

class ModguiHostBridgeTest {
    @Test
    fun setParameterPublishesTheCurrentInstanceAndGetReturnsCachedValue() {
        val submissions = mutableListOf<Submission>()
        val bridge = ModguiHostBridge(
            pathId = 41L,
            pluginInstanceId = 9001L,
            pluginInfo = pluginInfo(),
            submitParameter = { pathId, instanceId, portIndex, value ->
                submissions += Submission(pathId, instanceId, portIndex, value)
            },
        )

        bridge.setParameter("drive", 0.73f)

        assertEquals(listOf(Submission(41L, 9001L, 3, 0.73f)), submissions)
        assertEquals(0.73f, bridge.getParameter("drive"), 0.0f)
    }

    @Test
    fun laterInstanceIdIsUsedWithoutRetargetingEarlierCachedControl() {
        val submissions = mutableListOf<Submission>()
        val bridge = ModguiHostBridge(
            pathId = 7L,
            pluginInstanceId = 100L,
            pluginInfo = pluginInfo(),
            submitParameter = { pathId, instanceId, portIndex, value ->
                submissions += Submission(pathId, instanceId, portIndex, value)
            },
        )

        bridge.setParameter("drive", 0.25f)
        bridge.pluginInstanceId = 101L
        bridge.setParameter("drive", 0.5f)

        assertEquals(
            listOf(
                Submission(7L, 100L, 3, 0.25f),
                Submission(7L, 101L, 3, 0.5f),
            ),
            submissions,
        )
        assertEquals(0.5f, bridge.getParameter("drive"), 0.0f)
    }

    @Test
    fun snapshotUsesControlPortOrderAndRetargetedBridgePublishesWithResolvedInstance() {
        val submissions = mutableListOf<Submission>()
        val bridge = ModguiHostBridge(
            pathId = 19L,
            pluginInstanceId = 0L,
            pluginInfo = pluginInfo(),
            submitParameter = { pathId, instanceId, portIndex, value ->
                submissions += Submission(pathId, instanceId, portIndex, value)
            },
        )

        bridge.updateParameterSnapshot(floatArrayOf(0.31f, 0.82f))
        bridge.updateParameterSnapshot(floatArrayOf(0.44f, 0.68f))

        assertEquals(0.44f, bridge.getParameter("drive"), 0.0f)
        assertEquals(0.68f, bridge.getParameter("mix"), 0.0f)

        bridge.pluginInstanceId = 742L
        bridge.setParameter("mix", 0.95f)

        assertEquals(listOf(Submission(19L, 742L, 8, 0.95f)), submissions)
        assertEquals(0.95f, bridge.getParameter("mix"), 0.0f)
    }

    @Test
    fun nonControlAndUnknownSymbolsDoNotPublishOrAlterKnownCache() {
        val submissions = mutableListOf<Submission>()
        val bridge = ModguiHostBridge(
            pathId = 2L,
            pluginInstanceId = 3L,
            pluginInfo = pluginInfo(),
            submitParameter = { pathId, instanceId, portIndex, value ->
                submissions += Submission(pathId, instanceId, portIndex, value)
            },
        )

        bridge.setParameter("drive", 0.4f)
        bridge.setParameter("audio-input", 0.9f)
        bridge.setParameter("metadata", 0.8f)
        bridge.setParameter("not-a-port", 0.7f)

        assertEquals(1, submissions.size)
        assertEquals(0.4f, bridge.getParameter("drive"), 0.0f)
    }

    private fun pluginInfo() = PluginInfo(
        id = "urn:test:modgui",
        name = "Test Modgui",
        ports = listOf(
            PortInfo(index = 1, symbol = "audio-input", isAudio = true),
            PortInfo(index = 2, symbol = "metadata"),
            PortInfo(index = 3, symbol = "drive", name = "Drive", isControl = true),
            PortInfo(index = 8, symbol = "mix", name = "Mix", isControl = true),
        ),
    )


    private data class Submission(
        val pathId: Long,
        val instanceId: Long,
        val portIndex: Int,
        val value: Float,
    )
}
