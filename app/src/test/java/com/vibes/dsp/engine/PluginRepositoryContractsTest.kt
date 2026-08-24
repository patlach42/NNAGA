package com.vibes.dsp.engine

import java.net.URI
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import org.tomlj.Toml

class PluginRepositoryContractsTest {
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
    fun manifestParserPreservesPackageIdentityAndPayloadFields() {
        val manifest = parseRepositoryManifest(
            """
            schema = 1
            id = "example.plugin"
            name = "Example Plugin"
            version = "1.2.3"
            format = "lv2"
            description = "A deterministic test package"
            arch = ["arm64-v8a"]

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
        assertEquals("Test source", manifest.sourceName)
        assertEquals("https://plugins.example/repo/", manifest.repositoryRoot)
    }
}
