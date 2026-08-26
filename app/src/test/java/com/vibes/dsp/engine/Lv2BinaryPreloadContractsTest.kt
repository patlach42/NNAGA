package com.vibes.dsp.engine

import java.io.File
import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class Lv2BinaryPreloadContractsTest {
    @Test
    fun preloadsCanonicalDspBinariesAndIgnoresUiBinaryPredicates() {
        withBundle(
            ttl = """
                @prefix lv2: <http://lv2plug.in/ns/lv2core#> .
                @prefix ui: <http://lv2plug.in/ns/extensions/ui#> .
                <urn:test:plugin> lv2:binary <dsp/../dsp/libtest.so> ;
                    ui:binary <ui/test_ui.so> .
            """.trimIndent(),
            files = listOf("dsp/libtest.so", "ui/test_ui.so"),
        ) { bundle ->
            val loaded = mutableListOf<String>()

            preloadLv2Binaries(bundle, loaded::add)

            assertEquals(listOf(File(bundle, "dsp/libtest.so").canonicalPath), loaded)
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
    fun rejectsTraversalAndAbsoluteDspBinaryUris() {
        val cases = listOf(
            "traversal" to "../outside.so",
            "absolute URI" to "file:///tmp/outside.so",
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
                preloadLv2Binaries(bundle) { throw failure }
            }

            assertEquals(failure, error)
        }
    }

    private fun withBundle(
        ttl: String,
        files: List<String> = emptyList(),
        block: (File) -> Unit,
    ) {
        val bundle = Files.createTempDirectory("lv2-preload-contract").toFile()
        try {
            File(bundle, "manifest.ttl").writeText(ttl)
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
