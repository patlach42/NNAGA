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

    @Test
    fun formatGroupMapsLv2NativeJsfxAndEveryWineFormat() {
        val mixed = catalog + listOf(
            catalog.first().copy(
                id = "native-f",
                name = "Native Utility",
                format = "native",
            ),
            catalog.first().copy(
                id = "native-g",
                name = "Uppercase Native Utility",
                format = "NATIVE",
            ),
            catalog.first().copy(
                id = "jsfx-h",
                name = "JSFX Utility",
                format = "jsfx",
            ),
            catalog.first().copy(
                id = "wine-i",
                name = "Wine VST",
                format = "wine_archive",
            ),
            catalog.first().copy(
                id = "wine-j",
                name = "Wine Installer",
                format = "wine_installer",
            ),
        )

        assertIds(
            filterRepositoryPackages(
                mixed,
                query = "",
                manufacturer = null,
                tags = emptySet(),
                formatGroup = "LV2",
            ),
            "synth-b",
            "delay-a",
            "compressor-c",
            "reverb-d",
            "bass-e",
        )
        assertIds(
            filterRepositoryPackages(
                mixed,
                query = "",
                manufacturer = null,
                tags = emptySet(),
                formatGroup = "nAtIvE",
            ),
            "native-f",
            "native-g",
        )
        assertIds(
            filterRepositoryPackages(
                mixed,
                query = "",
                manufacturer = null,
                tags = emptySet(),
                formatGroup = "jsfx",
            ),
            "jsfx-h",
        )
        assertIds(
            filterRepositoryPackages(
                mixed,
                query = "",
                manufacturer = null,
                tags = emptySet(),
                formatGroup = "wInE",
            ),
            "wine-i",
            "wine-j",
        )
        assertIds(
            filterRepositoryPackages(
                mixed,
                query = "",
                manufacturer = null,
                tags = emptySet(),
                formatGroup = "VST",
            ),
        )
    }

    @Test
    fun formatGroupParticipatesInAndFiltering() {
        val mixed = catalog + catalog.first().copy(
            id = "wine-shimmer",
            name = "Wine Shimmer",
            format = "wine_archive",
            manufacturer = "Acme Audio",
            tags = listOf("Digital"),
        )

        assertIds(
            filterRepositoryPackages(
                mixed,
                query = "SHIMMER",
                manufacturer = "acme audio",
                tags = setOf("digital"),
                formatGroup = "Wine",
            ),
            "wine-shimmer",
        )
    }


    @Test
    fun paginationKeepsOrderAndHonorsPageBoundaries() {
        val packages = (1..51).map { index ->
            catalog.first().copy(id = "item-$index")
        }

        assertEquals(25, REPOSITORY_PAGE_SIZE)
        assertEquals(emptyList<String>(), paginateRepositoryPackages(packages, 0).map(RepositoryPackageItem::id))
        assertEquals((1..24).map { "item-$it" }, paginateRepositoryPackages(packages, 24).map(RepositoryPackageItem::id))
        assertEquals((1..25).map { "item-$it" }, paginateRepositoryPackages(packages, REPOSITORY_PAGE_SIZE).map(RepositoryPackageItem::id))
        assertEquals((1..26).map { "item-$it" }, paginateRepositoryPackages(packages, 26).map(RepositoryPackageItem::id))
        assertEquals((1..51).map { "item-$it" }, paginateRepositoryPackages(packages, 100).map(RepositoryPackageItem::id))
        assertEquals(emptyList<String>(), paginateRepositoryPackages(packages, -1).map(RepositoryPackageItem::id))
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
            source = "https://example.com/plugins/test-source",
            availableVersion = "1.0.0",
            installedVersion = null,
            description = description,
            status = RepositoryPackageStatus.Available,
            manufacturer = manufacturer,
            tags = tags,
        )
    }
}
