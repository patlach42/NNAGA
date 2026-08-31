package com.vibes.dsp.engine

import java.io.File
import java.nio.file.Files
import java.util.Properties
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class InstalledPluginFacetsTest {
    @Test
    fun readsLv2JsfxAndWineFacetsAndChoosesLatestVisibleVersion() {
        val actualFilesDir = Files.createTempDirectory("installed-plugin-facets").toFile()
        val filesDirLink = actualFilesDir.parentFile.resolve("${actualFilesDir.name}-link")
        try {
            Files.createSymbolicLink(filesDirLink.toPath(), actualFilesDir.toPath())

            val lv2Package = filesDirLink.resolve("plugin-repositories/installed/lv2/aidadsp")
            writeMetadata(
                lv2Package.resolve("1.0.0"),
                "format" to "lv2",
                "id" to "aidadsp",
                "name" to "AIDA-X",
                "manufacturer" to "Aida DSP",
                "tags" to " Dynamics \u001f Saturation ",
            )
            writeMetadata(
                lv2Package.resolve("2.0.0"),
                "format" to "lv2",
                "id" to "aidadsp",
                "name" to "AIDA-X",
                "manufacturer" to "Aida DSP",
                "tags" to "Dynamics\u001fSaturation\u001f dynamics ",
            )
            writeMetadata(
                lv2Package.resolve(".staging"),
                "format" to "lv2",
                "id" to "aidadsp",
                "name" to "staging",
                "manufacturer" to "Should not be read",
            )

            val jsfxPackage = filesDirLink.resolve("jsfx/Effects/repository/spectral/0.4.0")
            writeMetadata(
                jsfxPackage,
                "format" to "jsfx",
                "id" to "spectral",
                "name" to "Spectral JSFX",
                "manufacturer" to "Example Audio",
                "tags" to " Utility \u001f Delay ",
            )

            val winePackage = filesDirLink.resolve("plugin-repositories/installed/wine_vst3/wine-pack/3.1")
            writeMetadata(
                winePackage,
                "format" to "wine_vst3",
                "id" to "wine-pack",
                "name" to "Wine Pack",
                "manufacturer" to "Windows Vendor",
                "ownership.vstUuids" to " uuid-a , UUID-B, uuid-a ,  ",
            )

            val facets = readInstalledPluginFacets(filesDirLink)
            val byId = facets.associateBy { it.packageId }

            assertEquals(setOf("aidadsp", "spectral", "wine-pack"), byId.keys)

            val lv2 = byId.getValue("aidadsp")
            assertEquals("2.0.0", lv2.version)
            assertEquals(actualFilesDir.resolve("plugin-repositories/installed/lv2/aidadsp/2.0.0").canonicalFile, lv2.versionDirectory)
            assertEquals(listOf("Dynamics", "Saturation"), lv2.tags)
            assertTrue(lv2.vstUuids.isEmpty())

            val jsfx = byId.getValue("spectral")
            assertEquals("jsfx", jsfx.packageFormat)
            assertEquals("0.4.0", jsfx.version)
            assertEquals(listOf("Utility", "Delay"), jsfx.tags)

            assertEquals("wine_vst3", byId.getValue("wine-pack").packageFormat)
            assertEquals(setOf("uuid-a", "UUID-B"), byId.getValue("wine-pack").vstUuids)
        } finally {
            filesDirLink.delete()
            actualFilesDir.deleteRecursively()
        }
    }

    @Test
    fun ignoresMalformedAndIncompleteMetadataWithoutReturningPartialFacets() {
        val filesDir = Files.createTempDirectory("malformed-plugin-facets").toFile()
        try {
            val malformed = filesDir.resolve("plugin-repositories/installed/lv2/broken/1.0.0")
            malformed.mkdirs()
            File(malformed, REPOSITORY_METADATA_FILE).writeText("format=lv2\nname=Broken" + "\\" + "u\n")

            val incomplete = filesDir.resolve("jsfx/Effects/repository/incomplete/1.0.0")
            writeMetadata(
                incomplete,
                "format" to "jsfx",
                "id" to "incomplete",
                "name" to "Missing Manufacturer",
            )

            assertTrue(readInstalledPluginFacets(filesDir).isEmpty())
        } finally {
            filesDir.deleteRecursively()
        }
    }

    private fun writeMetadata(versionDirectory: File, vararg values: Pair<String, String>) {
        versionDirectory.mkdirs()
        Properties().apply {
            values.forEach { (key, value) -> setProperty(key, value) }
        }.let { properties ->
            File(versionDirectory, REPOSITORY_METADATA_FILE).outputStream().use { properties.store(it, null) }
        }
    }
}
