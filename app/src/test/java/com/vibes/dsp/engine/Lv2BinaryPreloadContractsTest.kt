package com.vibes.dsp.engine

import java.io.File
import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class Lv2BinaryPreloadContractsTest {
    @Test
    fun preloadsCanonicalNestedNamDspBinaryAndIgnoresUiBinaryPredicates() {
        withBundle(
            ttl = """
                @prefix lv2: <http://lv2plug.in/ns/lv2core#> .
                @prefix ui: <http://lv2plug.in/ns/extensions/ui#> .
                <urn:test:plugin> lv2:binary <./plugin.so> ;
                    ui:binary <ui/test_ui.so> .
            """.trimIndent(),
            ttlPath = "neural_amp_modeler.lv2/manifest.ttl",
            files = listOf(
                "neural_amp_modeler.lv2/plugin.so",
                "neural_amp_modeler.lv2/ui/test_ui.so",
            ),
        ) { bundle ->
            val loaded = mutableListOf<String>()

            preloadLv2Binaries(bundle, loaded::add)

            assertEquals(
                listOf(File(bundle, "neural_amp_modeler.lv2/plugin.so").canonicalPath),
                loaded,
            )
        }
    }

    @Test
    fun preloadsRewrittenCanonicalFileUriAfterRestartWhenEnabled() {
        withBundle(
            ttl = """
                @prefix lv2: <http://lv2plug.in/ns/lv2core#> .
                <urn:test:plugin> lv2:binary <./plugin.so> .
            """.trimIndent(),
            ttlPath = "neural_amp_modeler.lv2/manifest.ttl",
            files = listOf("neural_amp_modeler.lv2/plugin.so"),
        ) { bundle ->
            val binary = File(bundle, "neural_amp_modeler.lv2/plugin.so").canonicalFile
            File(bundle, "neural_amp_modeler.lv2/manifest.ttl").writeText(
                """
                    @prefix lv2: <http://lv2plug.in/ns/lv2core#> .
                    <urn:test:plugin> lv2:binary <${binary.toURI()}> .
                """.trimIndent(),
            )
            val loaded = mutableListOf<String>()

            assertThrows(IllegalArgumentException::class.java) {
                preloadLv2Binaries(bundle, loaded::add)
            }
            assertEquals(emptyList<String>(), loaded)

            preloadLv2Binaries(
                bundle,
                loaded::add,
                allowRewrittenFileUris = true,
            )

            assertEquals(listOf(binary.path), loaded)
        }
    }


    @Test
    fun rejectsMissingDspBinaryBeforeInvokingLoader() {
        withBundle(
            ttl = """
                @prefix lv2: <http://lv2plug.in/ns/lv2core#> .
                <urn:test:plugin> lv2:binary <dsp/missing.so> .
            """.trimIndent(),
        ) { bundle ->
            val loaded = mutableListOf<String>()

            assertThrows(IllegalStateException::class.java) {
                preloadLv2Binaries(bundle, loaded::add)
            }

            assertEquals(emptyList<String>(), loaded)
        }
    }

    @Test
    fun rejectsTraversalAbsoluteAndNonFileDspBinaryUris() {
        val cases = listOf(
            "traversal" to "../outside.so",
            "absolute URI" to "file:///tmp/outside.so",
            "non-file URI" to "https://example.test/plugin.so",
        )

        cases.forEach { (name, reference) ->
            withBundle(
                ttl = """
                    @prefix lv2: <http://lv2plug.in/ns/lv2core#> .
                    <urn:test:plugin> lv2:binary <$reference> .
                """.trimIndent(),
            ) { bundle ->
                assertThrows(name, IllegalArgumentException::class.java) {
                    preloadLv2Binaries(bundle, {})
                }
            }
        }
    }

    @Test
    fun skipsRewrittenFileUriOutsideBundleWhenEnabled() {
        val external = Files.createTempFile("lv2-external-", ".so").toFile()
        try {
            external.writeBytes(byteArrayOf(0x01))
            withBundle(
                ttl = """
                    @prefix lv2: <http://lv2plug.in/ns/lv2core#> .
                    <urn:test:plugin> lv2:binary <${external.canonicalFile.toURI()}> .
                """.trimIndent(),
            ) { bundle ->
                val loaded = mutableListOf<String>()

                preloadLv2Binaries(
                    bundle,
                    loaded::add,
                    allowRewrittenFileUris = true,
                )

                assertEquals(emptyList<String>(), loaded)
            }
        } finally {
            external.delete()
        }
    }

    @Test
    fun propagatesDspLoaderFailure() {
        val failure = IllegalStateException("synthetic loader failure")
        withBundle(
            ttl = """
                @prefix lv2: <http://lv2plug.in/ns/lv2core#> .
                <urn:test:plugin> lv2:binary <dsp/libtest.so> .
            """.trimIndent(),
            files = listOf("dsp/libtest.so"),
        ) { bundle ->
            val error = assertThrows(IllegalStateException::class.java) {
                preloadLv2Binaries(bundle, { throw failure })
            }

            assertEquals(failure, error)
        }
    }

    private fun withBundle(
        ttl: String,
        ttlPath: String = "manifest.ttl",
        files: List<String> = emptyList(),
        block: (File) -> Unit,
    ) {
        val bundle = Files.createTempDirectory("lv2-preload-contract").toFile()
        try {
            File(bundle, ttlPath).apply {
                parentFile?.mkdirs()
                writeText(ttl)
            }
            files.forEach { relativePath ->
                File(bundle, relativePath).apply {
                    parentFile?.mkdirs()
                    writeBytes(byteArrayOf(0x01))
                }
            }
            block(bundle)
        } finally {
            bundle.deleteRecursively()
        }
    }
}
