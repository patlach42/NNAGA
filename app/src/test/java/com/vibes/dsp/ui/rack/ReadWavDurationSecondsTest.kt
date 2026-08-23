package com.vibes.dsp.ui.rack

import java.io.ByteArrayOutputStream
import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

class ReadWavDurationSecondsTest {
    @Test
    fun readsDurationFromPcmDataBytes() {
        withTemporaryWav(
            riff(
                fmtChunk(sampleRate = 48_000, blockAlign = 4),
                chunk("data", ByteArray(48)),
            ),
        ) { file ->
            assertDuration(0.00025, file)
        }
    }

    @Test
    fun acceptsDataChunkBeforeFmtChunk() {
        withTemporaryWav(
            riff(
                chunk("data", ByteArray(24)),
                fmtChunk(sampleRate = 8_000, blockAlign = 2),
            ),
        ) { file ->
            assertDuration(0.0015, file)
        }
    }

    @Test
    fun usesFirstFmtAndDataChunksWhenDuplicatesAppear() {
        val firstFmt = fmtChunk(sampleRate = 8_000, blockAlign = 2)
        val secondFmt = fmtChunk(sampleRate = 48_000, blockAlign = 4)
        val firstData = chunk("data", ByteArray(24))
        val secondData = chunk("data", ByteArray(480))

        listOf(
            arrayOf(firstFmt, secondFmt, firstData, secondData),
            arrayOf(firstData, secondData, firstFmt, secondFmt),
        ).forEach { chunks ->
            withTemporaryWav(riff(*chunks)) { file ->
                assertDuration(0.0015, file)
            }
        }
    }

    @Test
    fun skipsOddSizedUnknownChunkPadding() {
        withTemporaryWav(
            riff(
                fmtChunk(sampleRate = 16_000, blockAlign = 2),
                chunk("JUNK", byteArrayOf(1, 2, 3)),
                chunk("data", ByteArray(32)),
            ),
        ) { file ->
            assertDuration(0.001, file)
        }
    }

    @Test
    fun rejectsInvalidContainerHeaders() {
        val invalidRiff = byteArrayOutputStream().apply {
            writeAscii("NOPE")
            writeLeInt(4)
            writeAscii("WAVE")
        }.toByteArray()
        val invalidWave = byteArrayOutputStream().apply {
            writeAscii("RIFF")
            writeLeInt(4)
            writeAscii("NOPE")
        }.toByteArray()

        withTemporaryWav(invalidRiff) { file ->
            assertNull(AudioImportDecoder.readWavDurationSeconds(file))
        }
        withTemporaryWav(invalidWave) { file ->
            assertNull(AudioImportDecoder.readWavDurationSeconds(file))
        }
    }

    @Test
    fun rejectsTruncatedChunkPayload() {
        val truncated = byteArrayOutputStream().apply {
            writeAscii("RIFF")
            writeLeInt(16)
            writeAscii("WAVE")
            writeAscii("fmt ")
            writeLeInt(16)
            write(byteArrayOf(1, 0, 1, 0))
        }.toByteArray()

        withTemporaryWav(truncated) { file ->
            assertNull(AudioImportDecoder.readWavDurationSeconds(file))
        }
    }

    @Test
    fun rejectsZeroSampleRateOrBlockAlign() {
        listOf(
            fmtChunk(sampleRate = 0, blockAlign = 2),
            fmtChunk(sampleRate = 8_000, blockAlign = 0),
        ).forEach { fmt ->
            withTemporaryWav(riff(fmt, chunk("data", ByteArray(8)))) { file ->
                assertNull(AudioImportDecoder.readWavDurationSeconds(file))
            }
        }
    }

    private fun assertDuration(expected: Double, file: File) {
        val actual = AudioImportDecoder.readWavDurationSeconds(file)
        assertNotNull(actual)
        assertEquals(expected, actual!!, 0.0)
    }

    private fun withTemporaryWav(bytes: ByteArray, block: (File) -> Unit) {
        val file = File.createTempFile("read_wav_duration_", ".wav")
        try {
            file.writeBytes(bytes)
            block(file)
        } finally {
            check(file.delete() || !file.exists()) { "Unable to delete temporary WAV: $file" }
        }
    }

    private fun riff(vararg chunks: ByteArray, format: String = "WAVE"): ByteArray {
        val body = byteArrayOutputStream().apply {
            writeAscii(format)
            chunks.forEach { write(it) }
        }.toByteArray()
        return byteArrayOutputStream().apply {
            writeAscii("RIFF")
            writeLeInt(body.size)
            write(body)
        }.toByteArray()
    }

    private fun fmtChunk(sampleRate: Int, blockAlign: Int): ByteArray =
        byteArrayOutputStream().apply {
            writeAscii("fmt ")
            writeLeInt(16)
            writeLeShort(1)
            writeLeShort(1)
            writeLeInt(sampleRate)
            writeLeInt(sampleRate * blockAlign)
            writeLeShort(blockAlign)
            writeLeShort(16)
        }.toByteArray()

    private fun chunk(id: String, payload: ByteArray): ByteArray =
        byteArrayOutputStream().apply {
            writeAscii(id)
            writeLeInt(payload.size)
            write(payload)
            if (payload.size % 2 != 0) write(0)
        }.toByteArray()

    private fun byteArrayOutputStream() = ByteArrayOutputStream()

    private fun ByteArrayOutputStream.writeAscii(value: String) {
        write(value.toByteArray(Charsets.US_ASCII))
    }

    private fun ByteArrayOutputStream.writeLeShort(value: Int) {
        write(value and 0xff)
        write((value ushr 8) and 0xff)
    }

    private fun ByteArrayOutputStream.writeLeInt(value: Int) {
        write(value and 0xff)
        write((value ushr 8) and 0xff)
        write((value ushr 16) and 0xff)
        write((value ushr 24) and 0xff)
    }
}
