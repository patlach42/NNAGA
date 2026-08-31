package com.vibes.dsp.ui.browser

import com.vibes.dsp.engine.InstalledPluginFacets
import com.vibes.dsp.engine.PluginInfo
import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class PluginBrowserFilterTest {
    @Test
    fun repositoryTagsMergeWithCategoryWithoutOtherOrCaseInsensitiveDuplicates() {
        val directory = Files.createTempDirectory("delay-facet").toFile()
        val plugin = plugin(id = "delay", name = "Delay", format = "LV2", originPath = directory.resolve("lib.so").path)
        try {
            val facet = facet(
                format = "LV2",
                id = "delay-package",
                version = "1.0.0",
                directory = directory,
                tags = listOf(" delay ", "Tone", "tone", " "),
            )

            val metadata = PluginMetadata(
                descriptions = emptyMap(),
                thumbnails = emptyMap(),
                authors = emptyMap(),
                categories = mapOf("Delay" to "DelayPlugin", "NoClass" to "UnknownPlugin"),
            )
            val entry = makePluginBrowserEntry(plugin, metadata, facets = listOf(facet))
            val otherEntry = makePluginBrowserEntry(
                plugin.copy(name = "NoClass"),
                metadata,
                facets = listOf(facet),
            )

            assertEquals(listOf("Delay", "Tone"), entry.tags)
            assertEquals(listOf("delay", "Tone"), otherEntry.tags)
            assertEquals("LV2", entry.type)
        } finally {
            directory.deleteRecursively()
        }

    }
    @Test
    fun repositoryFacetResolutionUsesVstUuidAndJsfxRuntimePrefix() {
        val root = Files.createTempDirectory("repository-resolution").toFile()
        try {
            val vstFacet = facet(
                format = "wine_archive",
                id = "wine-synth",
                version = "2.0",
                directory = root.resolve("vst"),
                vstUuids = setOf("uuid-a"),
            )
            val vst = plugin(id = "uuid-a", name = "Synth", format = "VST2")
            assertEquals(vstFacet, resolvePluginRepositoryFacet(vst, listOf(vstFacet)))

            val jsfxFacet = facet(
                format = "jsfx",
                id = "spectral",
                version = "0.4.0",
                directory = root.resolve("jsfx"),
            )
            val jsfx = plugin(
                id = "repository/spectral/0.4.0/Effects/Spectral.jsfx",
                name = "Spectral",
                format = "JSFX",
            )
            assertEquals(jsfxFacet, resolvePluginRepositoryFacet(jsfx, listOf(jsfxFacet)))
        } finally {
            root.deleteRecursively()
        }

    }
    @Test
    fun originResolutionMatchesCanonicalPathAndSupportsEveryDescriptorInOneNativeLibrary() {
        val root = Files.createTempDirectory("native-facet").toFile()
        try {
            val packageDirectory = root.resolve("aidadsp/1.0.0").apply { mkdirs() }
            Files.createSymbolicLink(root.resolve("aidadsp-link").toPath(), packageDirectory.toPath())
            val facet = facet("NATIVE", "aidadsp", "1.0.0", packageDirectory)
            val first = plugin("native-a", "AIDA-X", "NATIVE", root.resolve("aidadsp-link/libaida.so").path)
            val second = plugin("native-b", "AIDA-X Stereo", "NATIVE", root.resolve("aidadsp/1.0.0/libaida.so").path)
            val metadata = PluginMetadata(emptyMap(), emptyMap(), emptyMap(), emptyMap())

            assertEquals(facet, resolvePluginRepositoryFacet(first, listOf(facet)))
            assertEquals(facet, resolvePluginRepositoryFacet(second, listOf(facet)))
            assertEquals("Repository Vendor", makePluginBrowserEntry(first, metadata, listOf(facet)).author)
            assertEquals("Repository Vendor", makePluginBrowserEntry(second, metadata, listOf(facet)).author)
        } finally {
            root.deleteRecursively()
        }
    }

    @Test
    fun originMatchingDoesNotUseDisplayNameAndAmbiguousContainmentFallsBackToStaticMetadata() {
        val root = Files.createTempDirectory("origin-resolution").toFile()
        try {
            val packageDirectory = root.resolve("packages/aidadsp/1.0.0").apply { mkdirs() }
            val unrelatedDirectory = root.resolve("packages/aidadsp/1.0.0-other").apply { mkdirs() }
            val plugin = plugin("id-not-aidadsp", "AIDA-X", "LV2", root.resolve("packages/aidadsp/1.0.0-other/lib.so").path)
            val byName = facet("LV2", "aidadsp", "1.0.0", packageDirectory, manufacturer = "Repository Aida")
            val byOrigin = facet("LV2", "other", "1.0.0", unrelatedDirectory, manufacturer = "Repository Other")
            val metadata = PluginMetadata(emptyMap(), emptyMap(), mapOf("AIDA-X" to "Static Aida"), emptyMap())

            assertEquals(byOrigin, resolvePluginRepositoryFacet(plugin, listOf(byName, byOrigin)))
            assertEquals("Repository Other", makePluginBrowserEntry(plugin, metadata, listOf(byName, byOrigin)).author)

            val parent = root.resolve("ambiguous").apply { mkdirs() }
            val nested = parent.resolve("nested").apply { mkdirs() }
            val ambiguousPlugin = plugin("native", "Amp", "NATIVE", nested.resolve("plugin.so").path)
            val parentFacet = facet("NATIVE", "parent", "1", parent)
            val nestedFacet = facet("NATIVE", "nested", "1", nested)
            val staticMetadata = PluginMetadata(emptyMap(), emptyMap(), mapOf("Amp" to "Static Maker"), mapOf("Amp" to "AmplifierPlugin"))

            assertNull(resolvePluginRepositoryFacet(ambiguousPlugin, listOf(parentFacet, nestedFacet)))
            val fallback = makePluginBrowserEntry(ambiguousPlugin, staticMetadata, listOf(parentFacet, nestedFacet))
            assertEquals("Static Maker", fallback.author)
            assertEquals(listOf("Amplifier"), fallback.tags)
        } finally {
            root.deleteRecursively()
        }
    }

    @Test
    fun filtersCombineAuthorTypeAndAllTagsCaseInsensitivelyAndKeepFavoritesFirstOnce() {
        val alpha = entry("a", "Alpha", "Acme Audio", listOf("Delay", "Tone"), "VST3")
        val beta = entry("b", "beta", "acme audio", listOf("delay", "tone"), "vst3")
        val gamma = entry("g", "Gamma", "Other Audio", listOf("Delay", "Tone"), "LV2")
        val entries = listOf(gamma, beta, alpha)

        assertEquals(
            listOf("VST3:a", "vst3:b"),
            computeVisibleEntries(
                entries,
                PluginBrowserFilters(author = "ACME AUDIO", tags = setOf("DELAY", "tone"), type = "vSt3"),
                favorites = setOf("VST3:a", "VST3:missing"),
            ).map { it.plugin.fullId },
        )
        assertEquals(
            listOf("VST3:a", "vst3:b", "LV2:g"),
            computeVisibleEntries(entries, PluginBrowserFilters(), favorites = setOf("VST3:a")).map { it.plugin.fullId },
        )
    }

    @Test
    fun activeFiltersMayProduceAnEmptyVisibleList() {
        val entries = listOf(entry("a", "Alpha", "Acme", listOf("Delay", "Tone"), "LV2"))

        assertEquals(
            emptyList<String>(),
            computeVisibleEntries(
                entries,
                PluginBrowserFilters(author = "Acme", tags = setOf("Delay", "Missing"), type = "LV2"),
                favorites = emptySet(),
            ).map { it.plugin.fullId },
        )
    }

    private fun plugin(id: String, name: String, format: String, originPath: String = "") = PluginInfo(
        id = id,
        name = name,
        format = format,
        originPath = originPath,
    )

    private fun facet(
        format: String,
        id: String,
        version: String,
        directory: java.io.File,
        name: String = id,
        manufacturer: String = "Repository Vendor",
        tags: List<String> = emptyList(),
        vstUuids: Set<String> = emptySet(),
    ) = InstalledPluginFacets(format, id, version, directory.canonicalFile, name, manufacturer, tags, vstUuids)

    private fun entry(id: String, name: String, author: String, tags: List<String>, type: String) =
        PluginBrowserEntry(plugin(id, name, type), author, tags, type)
}
