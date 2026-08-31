package com.vibes.dsp.ui.live

import java.util.Locale
import org.junit.Assert.assertEquals
import org.junit.Test

class MixerTrackAbbreviationTest {
    @Test
    fun abbreviationUsesFirstThreeUppercaseConsonantLetters() {
        val cases = listOf(
            "Latin consonants" to ("Guitar" to "GTR"),
            "Cyrillic consonants" to ("Барабаны" to "БРБ"),
            "Punctuation and digits are ignored" to ("!g-2uitar?" to "GTR"),
        )

        cases.forEach { (description, input) ->
            assertEquals(description, input.second, mixerTrackAbbreviation(input.first, 0))
        }
    }

    @Test
    fun abbreviationFallsBackWhenNameHasNoEligibleLetters() {
        assertEquals("T7", mixerTrackAbbreviation("AEIOUАЕЁИОУЫЭЮЯЪЬ", 6))
    }

    @Test
    fun abbreviationIsDeterministicUnderTurkishDefaultLocale() {
        val previous = Locale.getDefault()
        try {
            Locale.setDefault(Locale("tr", "TR"))
            assertEquals("GTR", mixerTrackAbbreviation("guitar", 0))
        } finally {
            Locale.setDefault(previous)
        }
        assertEquals(previous, Locale.getDefault())
    }
}
