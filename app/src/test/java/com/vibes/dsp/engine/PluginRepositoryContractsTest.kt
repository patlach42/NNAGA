package com.vibes.dsp.engine

import java.net.URI
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import org.tomlj.Toml

class PluginRepositoryContractsTest {
    @Test
    fun validatesDeclaredContentLengthBoundaries() {
        val max = 1024L
        val url = "https://plugins.example/repo/index.toml"

        listOf(
            -1L to -1L,
            1L to 1L,
            max to max,
        ).forEach { (declared, expected) ->
            assertEquals(expected, validateDeclaredContentLength(declared, max, url))
        }

        listOf(0L, -2L, max + 1).forEach { declared ->
            val error = assertThrows(IllegalArgumentException::class.java) {
                validateDeclaredContentLength(declared, max, url)
            }
            assertEquals(
                "Invalid declared content length $declared (max $max) for $url",
                error.message,
            )
        }
    }

    @Test
    fun resolvesPayloadsInsideFileAndHttpsRepositoryRoots() {
        val cases = listOf(
            URI("file:///repo/index.toml") to URI("file:///repo/"),
            URI("https://plugins.example/repo/index.toml") to URI("https://plugins.example/repo/"),
        )

        cases.forEach { (index, root) ->
            assertEquals(
                root.resolve("payload/example.zip").toString(),
                resolveContainedRepositoryUrl(index.toString(), root, "payload/example.zip"),
            )
        }
    }
    @Test
    fun versionedIndexUsesQueryFreeRootForManifestAndPayloadResolution() {
        val index = "https://plugins.example/repo/index.toml?v=2026-08-23"
        val root = URI(index).resolve("./")

        assertEquals("https://plugins.example/repo/", root.toString())
        assertEquals(null, root.query)

        val manifest = resolveContainedRepositoryUrl(index, root, "packages/example/manifest.toml")
        val payload = resolveContainedRepositoryUrl(manifest, root, "../../payload/example/1.0.0.zip")

        assertEquals("https://plugins.example/repo/packages/example/manifest.toml", manifest)
        assertEquals("https://plugins.example/repo/payload/example/1.0.0.zip", payload)
        assertEquals(null, URI(manifest).query)
        assertEquals(null, URI(payload).query)
        assertEquals(true, URI(manifest).path.startsWith(root.path))
        assertEquals(true, URI(payload).path.startsWith(root.path))
    }


    @Test
    fun migratesStaleBuiltinSourceToVersionedUrlAndClearsError() {
        val staleBuiltin = RepositorySourceRecord(
            id = "builtin",
            name = "NNAGA Base",
            url = "https://raw.githubusercontent.com/patlach42/NNAGA/main/plugin-repository/index.toml",
            enabled = false,
            custom = false,
            lastError = "HTTP 404 https://raw.githubusercontent.com/patlach42/NNAGA/main/plugin-repository/index.toml",
        )
        val currentBuiltin = RepositorySourceRecord(
            id = "builtin",
            name = "NNAGA Base",
            url = "https://raw.githubusercontent.com/patlach42/NNAGA/main/plugin-repository/index.toml?v=2026-08-23",
            enabled = true,
            custom = false,
            lastError = null,
        )

        val migrated = migrateRepositorySources(listOf(staleBuiltin), currentBuiltin)

        assertEquals(listOf(currentBuiltin.copy(enabled = false)), migrated)
    }

    @Test
    fun leavesCustomSourceRecordUnchangedDuringBuiltinMigration() {
        val custom = RepositorySourceRecord(
            id = "custom-source",
            name = "Custom source",
            url = "https://plugins.example/custom/index.toml",
            enabled = true,
            custom = true,
            lastError = "HTTP 503 https://plugins.example/custom/index.toml",
        )
        val builtin = RepositorySourceRecord(
            id = "builtin",
            name = "NNAGA Base",
            url = "https://raw.githubusercontent.com/patlach42/NNAGA/main/plugin-repository/index.toml?v=2026-08-23",
            enabled = true,
            custom = false,
            lastError = null,
        )

        val migrated = migrateRepositorySources(listOf(custom), builtin)

        assertEquals(listOf(custom), migrated)
    }

    @Test
    fun rejectsEncodedTraversalAndCrossOriginPayloadUrls() {
        val httpsRoot = URI("https://plugins.example/repo/")
        val fileRoot = URI("file:///repo/")

        assertThrows(IllegalArgumentException::class.java) {
            resolveContainedRepositoryUrl(
                "https://plugins.example/repo/index.toml",
                httpsRoot,
                "payload/%2e%2e/%2e%2e/escape.zip",
            )
        }
        assertThrows(IllegalArgumentException::class.java) {
            resolveContainedRepositoryUrl(
                "https://plugins.example/repo/index.toml",
                httpsRoot,
                "https://attacker.example/payload.zip",
            )
        }
        assertThrows(IllegalArgumentException::class.java) {
            resolveContainedRepositoryUrl(
                "file:///repo/index.toml",
                fileRoot,
                "file:///tmp/escape.zip",
            )
        }
    }

    @Test
    fun malformedIndexTomlIsRejected() {
        val malformed = Toml.parse("schema = 1\nmanifests = [")

        assertThrows(IllegalArgumentException::class.java) {
            parseRepositoryIndex(malformed)
        }
    }

    @Test
    fun manifestParserPreservesPackageIdentityPayloadAndFacetFields() {
        val manifest = parseRepositoryManifest(
            """
            schema = 1
            id = "example.plugin"
            name = "Example Plugin"
            version = "1.2.3"
            format = "lv2"
            description = "A deterministic test package"
            arch = ["arm64-v8a"]
            manufacturer = "Acme Audio"
            tags = ["Delay", "Creative"]

            [payload]
            url = "payload/example.zip"
            sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
            size = 42
            kind = "archive"

            [install]
            entry = "Example.lv2"
            """.trimIndent(),
            source = "Test source",
            url = "https://plugins.example/repo/example.toml",
            repositoryRoot = "https://plugins.example/repo/",
        )

        assertEquals("example.plugin", manifest.id)
        assertEquals("1.2.3", manifest.version)
        assertEquals("lv2", manifest.format)
        assertEquals("payload/example.zip", manifest.payloadUrl)
        assertEquals(42L, manifest.payloadSize)
        assertEquals("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", manifest.payloadSha256)
        assertEquals("Example.lv2", manifest.entry)
        assertEquals("Acme Audio", manifest.manufacturer)
        assertEquals(listOf("Delay", "Creative"), manifest.tags)
        assertEquals("Test source", manifest.sourceName)
        assertEquals("https://plugins.example/repo/", manifest.repositoryRoot)
    }

    @Test
    fun legacyManifestDefaultsFacetMetadata() {
        val manifest = parseRepositoryManifest(
            """
            schema = 1
            id = "legacy.plugin"
            name = "Legacy Plugin"
            version = "1.0.0"
            format = "lv2"
            description = "A legacy package without facet metadata"
            arch = ["arm64-v8a"]

            [payload]
            url = "payload/legacy.zip"
            sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
            size = 42
            kind = "archive"

            [install]
            entry = "Legacy.lv2"
            """.trimIndent(),
            source = "Legacy source",
            url = "https://plugins.example/repo/legacy.toml",
            repositoryRoot = "https://plugins.example/repo/",
        )

        assertEquals("Unknown", manifest.manufacturer)
        assertEquals(emptyList<String>(), manifest.tags)
    }

    @Test
    fun parserRejectsPresentBlankManufacturer() {
        val malformed = """
            schema = 1
            id = "blank.manufacturer"
            name = "Blank Manufacturer"
            version = "1.0.0"
            format = "lv2"
            description = "Malformed facet fixture"
            manufacturer = ""
            tags = ["Delay"]

            [payload]
            url = "payload/blank-manufacturer.zip"
            sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
            size = 42
            kind = "archive"

            [install]
            entry = "Blank.lv2"
        """.trimIndent()

        assertThrows(IllegalArgumentException::class.java) {
            parseRepositoryManifest(
                malformed,
                source = "Test source",
                url = "https://plugins.example/repo/blank-manufacturer.toml",
                repositoryRoot = "https://plugins.example/repo/",
            )
        }
    }

    @Test
    fun parserRejectsNonStringTags() {
        val malformed = """
            schema = 1
            id = "numeric.tag"
            name = "Numeric Tag"
            version = "1.0.0"
            format = "lv2"
            description = "Malformed facet fixture"
            manufacturer = "Acme Audio"
            tags = ["Delay", 7]

            [payload]
            url = "payload/numeric-tag.zip"
            sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
            size = 42
            kind = "archive"

            [install]
            entry = "Numeric.lv2"
        """.trimIndent()

        assertThrows(IllegalArgumentException::class.java) {
            parseRepositoryManifest(
                malformed,
                source = "Test source",
                url = "https://plugins.example/repo/numeric-tag.toml",
                repositoryRoot = "https://plugins.example/repo/",
            )
        }
    }

    @Test
    fun facetMetadataValidationRejectsMalformedAndUnboundedValues() {
        val valid = parseRepositoryManifest(
            """
            schema = 1
            id = "valid.plugin"
            name = "Valid Plugin"
            version = "1.0.0"
            format = "lv2"
            description = "Validation fixture"
            manufacturer = "Acme Audio"
            tags = ["Delay"]

            [payload]
            url = "payload/valid.zip"
            sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
            size = 42
            kind = "archive"

            [install]
            entry = "Valid.lv2"
            """.trimIndent(),
            source = "Test source",
            url = "https://plugins.example/repo/valid.toml",
            repositoryRoot = "https://plugins.example/repo/",
        )

        listOf(
            "blank manufacturer" to valid.copy(manufacturer = ""),
            "overlong manufacturer" to valid.copy(manufacturer = "m".repeat(129)),
            "control-character manufacturer" to valid.copy(manufacturer = "Acme\u0000Audio"),
            "too many tags" to valid.copy(tags = List(33) { "tag" }),
            "blank tag" to valid.copy(tags = listOf("")),
            "overlong tag" to valid.copy(tags = listOf("t".repeat(65))),
            "control-character tag" to valid.copy(tags = listOf("Delay\u0000")),
        ).forEach { (case, manifest) ->
            assertThrows(case, IllegalArgumentException::class.java) {
                validateFacetMetadata(manifest)
            }
        }
    }
}
