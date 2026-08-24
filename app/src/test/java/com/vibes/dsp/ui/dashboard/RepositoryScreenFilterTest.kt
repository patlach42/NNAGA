package com.vibes.dsp.ui.dashboard

import org.junit.Assert.assertEquals
import org.junit.Test

class RepositoryScreenFilterTest {
    @Test
    fun blankFiltersReturnEveryPackageInCatalogOrder() {
        assertIds(
            filterRepositoryPackages(
                packages = catalog,
                query = "",
                manufacturer = null,
                tags = emptySet(),
            ),
            "synth-b",
            "delay-a",
            "compressor-c",
            "reverb-d",
            "bass-e",
        )
    }

    @Test
    fun queryMatchesEachSearchFieldIgnoringCase() {
        val cases = listOf(
            "name" to ("pOlY" to listOf("synth-b")),
            "description" to ("sHiMmEr" to listOf("synth-b", "delay-a")),
            "manufacturer" to ("tOnE fOrGe" to listOf("compressor-c", "reverb-d")),
            "tag" to ("sTeReO" to listOf("delay-a", "compressor-c", "reverb-d")),
        )

        cases.forEach { (field, queryAndExpected) ->
            val (query, expectedIds) = queryAndExpected
            assertEquals(
                "$field query",
                expectedIds,
                filterRepositoryPackages(catalog, query, manufacturer = null, tags = emptySet())
                    .map(RepositoryPackageItem::id),
            )
        }
    }

    @Test
    fun manufacturerSelectionIsExactAndCaseInsensitive() {
        assertIds(
            filterRepositoryPackages(catalog, "", manufacturer = " aCmE aUdIo ", tags = emptySet()),
            "synth-b",
            "delay-a",
            "bass-e",
        )
        assertIds(
            filterRepositoryPackages(catalog, "", manufacturer = "Acme", tags = emptySet()),
        )
    }

    @Test
    fun selectedTagsRequireEveryTagIgnoringCase() {
        assertIds(
            filterRepositoryPackages(
                catalog,
                query = "",
                manufacturer = null,
                tags = setOf(" AnAloG ", "mOnOpHoNiC"),
            ),
            "synth-b",
            "bass-e",
        )
    }

    @Test
    fun queryManufacturerAndTagsAreCombinedWithAnd() {
        assertIds(
            filterRepositoryPackages(
                catalog,
                query = "SHIMMER",
                manufacturer = "acme audio",
                tags = setOf("digital"),
            ),
            "delay-a",
        )
    }

    @Test
    fun unmatchedQueryProducesNoPackages() {
        assertIds(
            filterRepositoryPackages(
                catalog,
                query = "does-not-exist",
                manufacturer = null,
                tags = emptySet(),
            ),
        )
    }

    private fun assertIds(actual: List<RepositoryPackageItem>, vararg expectedIds: String) {
        assertEquals(expectedIds.toList(), actual.map(RepositoryPackageItem::id))
    }

    private companion object {
        val catalog = listOf(
            packageItem(
                id = "synth-b",
                name = "Poly Synth",
                description = "Bright shimmer pad",
                manufacturer = "Acme Audio",
                tags = listOf("Analog", "Monophonic", "Synth"),
            ),
            packageItem(
                id = "delay-a",
                name = "Echo Chamber",
                description = "Warm shimmer repeats",
                manufacturer = "Acme Audio",
                tags = listOf("Digital", "Stereo"),
            ),
            packageItem(
                id = "compressor-c",
                name = "Opto Comp",
                description = "Smooth dynamics",
                manufacturer = "Tone Forge",
                tags = listOf("Analog", "Stereo"),
            ),
            packageItem(
                id = "reverb-d",
                name = "Hall Bloom",
                description = "Ambient space",
                manufacturer = "Tone Forge",
                tags = listOf("Digital", "Stereo", "Ambient"),
            ),
            packageItem(
                id = "bass-e",
                name = "Sub Pulse",
                description = "Deep low-end",
                manufacturer = "ACME AUDIO",
                tags = listOf("Analog", "Monophonic", "Bass"),
            ),
        )

        fun packageItem(
            id: String,
            name: String,
            description: String,
            manufacturer: String,
            tags: List<String>,
        ) = RepositoryPackageItem(
            id = id,
            name = name,
            format = "lv2",
            sourceName = "test-source",
            availableVersion = "1.0.0",
            installedVersion = null,
            description = description,
            status = RepositoryPackageStatus.Available,
            manufacturer = manufacturer,
            tags = tags,
        )
    }
}
