package com.vibes.dsp.tweaks

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * An affinity mask is a hex bitmask, and getting the shift or base wrong pins
 * an interrupt to the wrong cluster while still looking like the tweak worked.
 */
class PerformanceTweaksTest {

    @Test
    fun `picks every cpu at the highest capacity`() {
        // The reference device: six cores at 762 and two at 1024.
        val topology = """
            0 762
            1 762
            2 762
            3 762
            4 762
            5 762
            6 1024
            7 1024
        """.trimIndent()
        // CPUs 6 and 7 are bits 6 and 7, so 0xc0.
        assertEquals("c0", PerformanceTweaks.bigCoreMaskOf(topology))
    }

    @Test
    fun `handles a single fast core`() {
        assertEquals("80", PerformanceTweaks.bigCoreMaskOf("0 500\n7 1024"))
    }

    @Test
    fun `treats a uniform machine as all cores`() {
        assertEquals("f", PerformanceTweaks.bigCoreMaskOf("0 1024\n1 1024\n2 1024\n3 1024"))
    }

    @Test
    fun `refuses to guess when the topology is unreadable`() {
        // Returning a mask covering every CPU would claim they are all fast
        // and pin an interrupt on that basis. An empty mask means "do not
        // apply", which is the only honest answer without capacities.
        assertEquals("", PerformanceTweaks.bigCoreMaskOf(""))
        assertEquals("", PerformanceTweaks.bigCoreMaskOf("garbage\nlines"))
        assertEquals("", PerformanceTweaks.bigCoreMaskOf("0 0\n1 0"))
    }
}
