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
