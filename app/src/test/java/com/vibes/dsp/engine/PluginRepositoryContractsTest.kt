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
    fun resolvesExternalHttpsPayloadsButKeepsManifestContainmentStrict() {
        val root = URI("https://plugins.example/repo/")
        val index = "https://plugins.example/repo/index.toml"
        val externalPayload =
            "https://raw.githubusercontent.com/JoepVanlier/JSFX/7d9b1456fbe4543406a4e927c89453a212cab3eb/Basics/BandJoiner.jsfx"

        assertEquals(
            externalPayload,
            resolveRepositoryPayloadUrl(index, root, externalPayload, allowExternalHttps = true),
        )
        assertEquals(
            "https://plugins.example/repo/payload/example.jsfx",
            resolveRepositoryPayloadUrl(
                index,
                root,
                "payload/example.jsfx",
                allowExternalHttps = true,
            ),
        )

        listOf(
            "non-HTTPS absolute URL" to
                "http://raw.githubusercontent.com/JoepVanlier/JSFX/main/Basics/BandJoiner.jsfx",
            "file URL" to "file:///tmp/example.jsfx",
            "protocol-relative URL" to "//attacker.example/example.jsfx",
            "encoded traversal" to "https://plugins.example/repo/%2e%2e/escape.jsfx",
            "credential-bearing URL" to "https://user:secret@attacker.example/example.jsfx",
            "query-bearing URL" to "https://attacker.example/example.jsfx?ref=main",
            "fragment-bearing URL" to "https://attacker.example/example.jsfx#fragment",
        ).forEach { (case, payload) ->
            assertThrows(case, IllegalArgumentException::class.java) {
                resolveRepositoryPayloadUrl(index, root, payload, allowExternalHttps = true)
            }
        }

        assertThrows(IllegalArgumentException::class.java) {
            resolveRepositoryPayloadUrl(index, root, externalPayload, allowExternalHttps = false)
        }
        listOf(
            "https://attacker.example/manifest.toml",
            "http://attacker.example/manifest.toml",
            "file:///tmp/manifest.toml",
        ).forEach { manifest ->
            assertThrows(IllegalArgumentException::class.java) {
                resolveContainedRepositoryUrl(index, root, manifest)
            }
        }
    }

    @Test
    fun jsfxManifestAcceptsDependencyCompleteFilesAndLegacyFilePayloads() {
        val legacy = jsfxManifest()
        validateRepositoryManifest(legacy)

        val files = jsfxFilesManifest()
        validateRepositoryManifest(files)

        val extensionlessPath = "Effects/example"
        val extensionlessFiles = files.copy(
            entry = extensionlessPath,
            files = files.files.mapIndexed { index, file ->
                if (index == 0) file.copy(path = extensionlessPath) else file
            },
        )
        validateRepositoryManifest(extensionlessFiles)

        listOf(
            "extension-bearing .jsfx-inc entry" to "Effects/example.jsfx-inc",
            "extension-bearing .txt entry" to "Effects/example.txt",
            "extension-bearing .rpl entry" to "Effects/example.rpl",
        ).forEach { (case, entry) ->
            val declaredExtensionBearingEntry = files.copy(
                entry = entry,
                files = files.files.mapIndexed { index, file ->
                    if (index == 0) file.copy(path = entry) else file
                },
            )
            assertThrows(case, IllegalArgumentException::class.java) {
                validateRepositoryManifest(declaredExtensionBearingEntry)
            }
        }

        assertThrows(IllegalArgumentException::class.java) {
            validateRepositoryManifest(legacy.copy(kind = "archive"))
        }

        listOf(
            "installer",
            "directory",
            "zip",
            "raw",
            "unknown",
            "ARCHIVE",
            "",
        ).forEach { kind ->
            assertThrows("unsupported JSFX payload kind: $kind", IllegalArgumentException::class.java) {
                validateRepositoryManifest(legacy.copy(kind = kind))
            }
        }

        listOf(
            "non-jsfx format" to legacy.copy(format = "JSFX"),
            "extensionless file entry" to legacy.copy(entry = "Effects/example"),
            "uppercase entry extension" to legacy.copy(entry = "Effects/example.JSFX"),
            "absolute entry" to legacy.copy(entry = "/Effects/example.jsfx"),
            "traversal entry" to legacy.copy(entry = "Effects/../escape.jsfx"),
            "backslash traversal entry" to legacy.copy(entry = "Effects\\..\\escape.jsfx"),
            "empty entry" to legacy.copy(entry = ""),
            "directory entry" to legacy.copy(entry = "Effects/"),
        ).forEach { (case, manifest) ->
            assertThrows(case, IllegalArgumentException::class.java) {
                validateRepositoryManifest(manifest)
            }
        }
    }

    @Test
    fun jsfxFilesManifestRequiresDirectGithubRawUrls() {
        val valid = jsfxFilesManifest()
        val direct = valid.files.first().copy(
            url = "https://raw.githubusercontent.com/author/JSFX/commit/Basics/BandJoiner.jsfx",
        )
        validateRepositoryManifest(valid.copy(files = listOf(direct, valid.files[1])))

        val redirecting = direct.copy(
            url = "https://github.com/author/JSFX/raw/commit/Basics/BandJoiner.jsfx",
        )
        assertThrows(IllegalArgumentException::class.java) {
            validateRepositoryManifest(valid.copy(files = listOf(redirecting, valid.files[1])))
        }
    }

    @Test
    fun jsfxFilesManifestRejectsMissingDuplicateUnsafeAndInvalidFileDeclarations() {
        val valid = jsfxFilesManifest()
        val first = valid.files.first()

        listOf(
            "missing files" to jsfxFilesManifest(files = emptyList()),
            "duplicate destination" to jsfxFilesManifest(files = listOf(first, first)),
            "entry not declared" to jsfxFilesManifest(entry = "Effects/missing.jsfx"),
            "absolute destination" to jsfxFilesManifest(files = listOf(first.copy(path = "/Effects/example.jsfx"))),
            "parent traversal destination" to jsfxFilesManifest(files = listOf(first.copy(path = "../escape.jsfx"))),
            "nested traversal destination" to jsfxFilesManifest(files = listOf(first.copy(path = "Effects/../escape.jsfx"))),
            "backslash traversal destination" to jsfxFilesManifest(files = listOf(first.copy(path = "Effects\\..\\escape.jsfx"))),
            "empty destination" to jsfxFilesManifest(files = listOf(first.copy(path = ""))),
            "directory destination" to jsfxFilesManifest(files = listOf(first.copy(path = "Effects/"))),
            "non-HTTPS URL" to jsfxFilesManifest(files = listOf(first.copy(url = "http://author.example/example.jsfx"))),
            "relative URL" to jsfxFilesManifest(files = listOf(first.copy(url = "Basics/example.jsfx"))),
            "URL query" to jsfxFilesManifest(files = listOf(first.copy(url = "https://author.example/example.jsfx?ref=main"))),
            "URL fragment" to jsfxFilesManifest(files = listOf(first.copy(url = "https://author.example/example.jsfx#latest"))),
            "invalid hash" to jsfxFilesManifest(files = listOf(first.copy(sha256 = "not-a-sha256"))),
            "zero size" to jsfxFilesManifest(files = listOf(first.copy(size = 0))),
            "negative size" to jsfxFilesManifest(files = listOf(first.copy(size = -1))),
            "oversized file" to jsfxFilesManifest(files = listOf(first.copy(size = 512L * 1024 * 1024 + 1))),
        ).forEach { (case, manifest) ->
            assertThrows(case, IllegalArgumentException::class.java) {
                validateRepositoryManifest(manifest)
            }
        }
    }

    @Test
    fun parsesAndValidatesDependencyCompleteJsfxFiles() {
        val manifest = parseRepositoryManifest(
            """
            schema = 1
            id = "example.jsfx"
            name = "Example JSFX"
            version = "1.0.0"
            format = "jsfx"
            description = "A deterministic JSFX fixture"
            arch = ["arm64-v8a"]
            manufacturer = "Acme Audio"
            tags = ["JSFX"]

            [payload]
            kind = "files"

            [[payload.files]]
            url = "https://raw.githubusercontent.com/author/JSFX/commit/Basics/BandJoiner.jsfx"
            sha256 = "0000000000000000000000000000000000000000000000000000000000000000"
            size = 42
            path = "Effects/example.jsfx"

            [[payload.files]]
            url = "https://raw.githubusercontent.com/author/JSFX/commit/Basics/Utilities.jsfx-inc"
            sha256 = "1111111111111111111111111111111111111111111111111111111111111111"
            size = 7
            path = "Effects/Utilities.jsfx-inc"

            [install]
            entry = "Effects/example.jsfx"
            """.trimIndent(),
            source = "Test source",
            url = "https://plugins.example/repo/example.toml",
            repositoryRoot = "https://plugins.example/repo/",
        )

        assertEquals("files", manifest.kind)
        assertEquals(49L, manifest.payloadSize)
        assertEquals(
            listOf(
                RepoManifestFile(
                    "https://raw.githubusercontent.com/author/JSFX/commit/Basics/BandJoiner.jsfx",
                    "0000000000000000000000000000000000000000000000000000000000000000",
                    42,
                    "Effects/example.jsfx",
                ),
                RepoManifestFile(
                    "https://raw.githubusercontent.com/author/JSFX/commit/Basics/Utilities.jsfx-inc",
                    "1111111111111111111111111111111111111111111111111111111111111111",
                    7,
                    "Effects/Utilities.jsfx-inc",
                ),
            ),
            manifest.files,
        )
        validateRepositoryManifest(manifest)
    }

    private fun jsfxFilesManifest(
        files: List<RepoManifestFile> = listOf(
            RepoManifestFile(
                url = "https://raw.githubusercontent.com/author/JSFX/commit/Basics/BandJoiner.jsfx",
                sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
                size = 42,
                path = "Effects/example.jsfx",
            ),
            RepoManifestFile(
                url = "https://raw.githubusercontent.com/author/JSFX/commit/Basics/Utilities.jsfx-inc",
                sha256 = "1111111111111111111111111111111111111111111111111111111111111111",
                size = 7,
                path = "Effects/Utilities.jsfx-inc",
            ),
        ),
        entry: String = "Effects/example.jsfx",
    ) = PluginRepositoryService.RepoManifest(
        schema = 1,
        id = "example.jsfx",
        name = "Example JSFX",
        version = "1.0.0",
        format = "jsfx",
        description = "A deterministic JSFX fixture",
        payloadUrl = "",
        payloadSha256 = "",
        payloadSize = files.sumOf { it.size },
        entry = entry,
        kind = "files",
        files = files,
        arch = listOf("arm64-v8a"),
        sourceName = "Test source",
        sourceUrl = "https://plugins.example/repo/example.toml",
        repositoryRoot = "https://plugins.example/repo/",
        manufacturer = "Acme Audio",
        tags = listOf("JSFX"),
    )

    private fun jsfxManifest() = PluginRepositoryService.RepoManifest(
        schema = 1,
        id = "example.jsfx",
        name = "Example JSFX",
        version = "1.0.0",
        format = "jsfx",
        description = "A deterministic JSFX fixture",
        payloadUrl =
            "https://raw.githubusercontent.com/JoepVanlier/JSFX/7d9b1456fbe4543406a4e927c89453a212cab3eb/Basics/BandJoiner.jsfx",
        payloadSha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        payloadSize = 42,
        entry = "Effects/example.jsfx",
        kind = "file",
        arch = listOf("arm64-v8a"),
        sourceName = "Test source",
        sourceUrl = "https://plugins.example/repo/example.toml",
        repositoryRoot = "https://plugins.example/repo/",
        manufacturer = "Acme Audio",
        tags = listOf("JSFX"),
    )

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
