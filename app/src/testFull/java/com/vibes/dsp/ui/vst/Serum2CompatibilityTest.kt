package com.vibes.dsp.ui.vst

import org.junit.Assert.assertEquals
import org.junit.Test

class Serum2CompatibilityTest {
    @Test
    fun patchPreferencesEnablesBothRendererSettingsWithoutChangingOtherBytes() {
        val raw = """{
  "name": "Serum",
  "Disable DirectComposition": false,
  "Disable Partial Redraw": false
}"""
        val expected = """{
  "name": "Serum",
  "Disable DirectComposition": true,
  "Disable Partial Redraw": true
}"""

        val patched = Serum2Compatibility.patchPreferences(raw, hasExistingMembers = true)

        assertEquals(expected, patched)
    }

    @Test
    fun patchPreferencesIgnoresNestedRendererSettingAndAppendsMissingTopLevelSetting() {
        val raw = """{
  "nested": {
    "Disable DirectComposition": false
  },
  "Disable Partial Redraw": false
}"""
        val expected = """{
  "nested": {
    "Disable DirectComposition": false
  },
  "Disable Partial Redraw": true,
  "Disable DirectComposition": true
}"""

        val patched = Serum2Compatibility.patchPreferences(raw, hasExistingMembers = true)

        assertEquals(expected, patched)
    }

    @Test
    fun patchPreferencesLeavesAlreadyEnabledPreferencesByteIdentical() {
        val raw = """{
    "Disable DirectComposition" : true,
    "Disable Partial Redraw":true,
    "name": "Serum"
}
"""

        assertEquals(raw, Serum2Compatibility.patchPreferences(raw, hasExistingMembers = true))
    }

    @Test
    fun patchPreferencesAppendsMissingSettingsToNonEmptyObject() {
        val raw = """{
    "name": "Serum",
    "version": 2
}"""
        val expected = """{
    "name": "Serum",
    "version": 2,
    "Disable DirectComposition": true,
    "Disable Partial Redraw": true
}"""

        val patched = Serum2Compatibility.patchPreferences(raw, hasExistingMembers = true)

        assertEquals(expected, patched)
    }

    @Test
    fun patchPreferencesAppendsMissingSettingsToEmptyObject() {
        val raw = "{}"
        val expected = """{
"Disable DirectComposition": true,
"Disable Partial Redraw": true
}"""

        val patched = Serum2Compatibility.patchPreferences(raw, hasExistingMembers = false)

        assertEquals(expected, patched)
    }

    @Test
    fun patchPreferencesPreservesCrLfAndExistingIndentationWhenAppending() {
        val raw = "{\r\n\t\"name\": \"Serum\",\r\n\t\"version\": 2\r\n}"
        val expected =
            "{\r\n\t\"name\": \"Serum\",\r\n\t\"version\": 2,\r\n" +
                "\t\"Disable DirectComposition\": true,\r\n" +
                "\t\"Disable Partial Redraw\": true\r\n}"

        val patched = Serum2Compatibility.patchPreferences(raw, hasExistingMembers = true)

        assertEquals(expected, patched)
    }
}
