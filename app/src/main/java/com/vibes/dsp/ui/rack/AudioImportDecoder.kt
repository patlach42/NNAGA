package com.vibes.dsp.ui.rack

import android.content.Context
import android.media.AudioFormat
import android.media.MediaCodec
import android.media.MediaExtractor
import android.media.MediaFormat
import android.net.Uri
import java.io.BufferedOutputStream
import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Decodes Android-supported compressed audio to the PCM WAV consumed by native WavIO. */
internal object AudioImportDecoder {
    private const val IO_TIMEOUT_US = 10_000L
    private const val MAX_CHANNELS = 2
    /**
     * Reads PCM duration from a RIFF/WAVE file without loading audio data.
     * Chunk payloads are skipped by seeking, so memory use is constant.
     */
    internal fun readWavDurationSeconds(file: File): Double? {
        return runCatching {
            RandomAccessFile(file, "r").use { raf ->
                val fileLength = raf.length()
                if (fileLength < 12L) return@use null
                val header = ByteArray(12)
                raf.readFully(header)
                if (!header.copyOfRange(0, 4).contentEquals("RIFF".toByteArray()) ||
                    !header.copyOfRange(8, 12).contentEquals("WAVE".toByteArray())
                ) return@use null
                val riffSize = leUInt32(header, 4)
                val riffEnd = 8L + riffSize
                if (riffEnd < 12L || riffEnd > fileLength) return@use null
                var sampleRate = 0L
                var blockAlign = 0L
                var dataBytes = 0L
                var fmtFound = false
                var dataFound = false
                val chunkHeader = ByteArray(8)
                val fmt = ByteArray(16)
                while (raf.filePointer + 8L <= riffEnd) {
                    raf.readFully(chunkHeader)
                    val chunkSize = leUInt32(chunkHeader, 4)
                    val payloadStart = raf.filePointer
                    val payloadEnd = payloadStart + chunkSize
                    if (payloadEnd < payloadStart || payloadEnd > riffEnd) return@use null
                    when {
                        !fmtFound &&
                            chunkHeader[0] == 'f'.code.toByte() && chunkHeader[1] == 'm'.code.toByte() &&
                            chunkHeader[2] == 't'.code.toByte() && chunkHeader[3] == ' '.code.toByte() -> {
                            if (chunkSize < 16L) return@use null
                            raf.readFully(fmt)
                            sampleRate = leUInt32(fmt, 4)
                            blockAlign = leUInt16(fmt, 12).toLong()
                            fmtFound = true
                        }
                        !dataFound &&
                            chunkHeader[0] == 'd'.code.toByte() && chunkHeader[1] == 'a'.code.toByte() &&
                            chunkHeader[2] == 't'.code.toByte() && chunkHeader[3] == 'a'.code.toByte() -> {
                            dataBytes = chunkSize
                            dataFound = true
                        }
                    }
                    val paddedEnd = payloadEnd + (chunkSize and 1L)
                    if (paddedEnd < payloadEnd || paddedEnd > riffEnd) return@use null
                    raf.seek(paddedEnd)
                    if (fmtFound && dataFound) break
                }
                if (!fmtFound || !dataFound) return@use null
                if (sampleRate <= 0L || blockAlign <= 0L || dataBytes <= 0L) {
                    null
                } else {
                    (dataBytes.toDouble() / (sampleRate.toDouble() * blockAlign.toDouble()))
                        .takeIf { it.isFinite() && it > 0.0 }
                }
            }
        }.getOrNull()
    }

    private fun leUInt16(bytes: ByteArray, offset: Int): Int =
        (bytes[offset].toInt() and 0xff) or ((bytes[offset + 1].toInt() and 0xff) shl 8)

    private fun leUInt32(bytes: ByteArray, offset: Int): Long =
        (bytes[offset].toLong() and 0xffL) or
            ((bytes[offset + 1].toLong() and 0xffL) shl 8) or
            ((bytes[offset + 2].toLong() and 0xffL) shl 16) or
            ((bytes[offset + 3].toLong() and 0xffL) shl 24)


    fun copyOrDecode(context: Context, uri: Uri, output: File) {
        val mime = context.contentResolver.getType(uri)?.lowercase()
        val directWav = mime == "audio/wav" || mime == "audio/x-wav" ||
            uri.lastPathSegment?.substringBefore('?')?.lowercase()?.endsWith(".wav") == true
        if (directWav) {
            context.contentResolver.openInputStream(uri)?.use { input ->
                output.outputStream().use { input.copyTo(it) }
            } ?: throw IllegalArgumentException("Unable to open selected audio")
        } else {
            decode(context, uri, output)
        }
    }

    private fun decode(context: Context, uri: Uri, output: File) {
        val extractor = MediaExtractor()
        var codec: MediaCodec? = null
        try {
            val afd = context.contentResolver.openAssetFileDescriptor(uri, "r")
                ?: throw IllegalArgumentException("Unable to open selected audio")
            afd.use { extractor.setDataSource(it.fileDescriptor, it.startOffset, it.length) }
            val trackIndex = (0 until extractor.trackCount).firstOrNull { index ->
                extractor.getTrackFormat(index).getString(MediaFormat.KEY_MIME)?.startsWith("audio/") == true
            } ?: throw IllegalArgumentException("No audio stream found; choose an MP3, OGG, M4A, or WAV file")
            extractor.selectTrack(trackIndex)
            val inputFormat = extractor.getTrackFormat(trackIndex)
            val inputMime = inputFormat.getString(MediaFormat.KEY_MIME)
                ?: throw IllegalArgumentException("Unsupported audio format")
            codec = MediaCodec.createDecoderByType(inputMime)
            codec.configure(inputFormat, null, null, 0)
            codec.start()

            var sampleRate = inputFormat.intOrDefault(MediaFormat.KEY_SAMPLE_RATE, 0)
            var channels = inputFormat.intOrDefault(MediaFormat.KEY_CHANNEL_COUNT, 0)
            require(sampleRate > 0 && channels in 1..MAX_CHANNELS) { "Audio stream has invalid format" }
            var pcmEncoding = AudioFormat.ENCODING_PCM_16BIT
            var inputDone = false
            var outputDone = false
            var pcmBytes = 0L
            output.parentFile?.mkdirs()
            BufferedOutputStream(output.outputStream()).use { stream ->
                stream.write(ByteArray(44))
                val info = MediaCodec.BufferInfo()
                while (!outputDone) {
                    if (!inputDone) {
                        val inputIndex = codec.dequeueInputBuffer(IO_TIMEOUT_US)
                        if (inputIndex >= 0) {
                            val inputBuffer = codec.getInputBuffer(inputIndex)
                                ?: error("Decoder input buffer unavailable")
                            val sampleSize = extractor.readSampleData(inputBuffer, 0)
                            if (sampleSize < 0) {
                                codec.queueInputBuffer(inputIndex, 0, 0, 0L, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                                inputDone = true
                            } else {
                                codec.queueInputBuffer(inputIndex, 0, sampleSize, extractor.sampleTime, 0)
                                extractor.advance()
                            }
                        }
                    }
                    when (val outputIndex = codec.dequeueOutputBuffer(info, IO_TIMEOUT_US)) {
                        MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                            val format = codec.outputFormat
                            sampleRate = format.intOrDefault(MediaFormat.KEY_SAMPLE_RATE, sampleRate)
                            channels = format.intOrDefault(MediaFormat.KEY_CHANNEL_COUNT, channels)
                            pcmEncoding = format.intOrDefault(MediaFormat.KEY_PCM_ENCODING, AudioFormat.ENCODING_PCM_16BIT)
                            require(sampleRate > 0 && channels in 1..MAX_CHANNELS) { "Decoder returned invalid format" }
                        }
                        MediaCodec.INFO_TRY_AGAIN_LATER -> Unit
                        else -> if (outputIndex >= 0) {
                            val buffer = codec.getOutputBuffer(outputIndex)
                            if (buffer != null && info.size > 0) {
                                buffer.position(info.offset)
                                buffer.limit(info.offset + info.size)
                                pcmBytes += writePcm16(buffer, pcmEncoding, stream)
                            }
                            codec.releaseOutputBuffer(outputIndex, false)
                            if ((info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) outputDone = true
                        }
                    }
                }
                stream.flush()
            }
            require(pcmBytes > 0) { "Audio decoder produced no samples; the file may be corrupt" }
            writeWavHeader(output, pcmBytes, sampleRate, channels)
        } catch (e: IllegalArgumentException) {
            output.delete()
            throw e
        } catch (e: Exception) {
            output.delete()
            throw IllegalArgumentException("Unable to decode audio; the file may be corrupt or unsupported", e)
        } finally {
            runCatching { codec?.stop() }
            runCatching { codec?.release() }
            extractor.release()
        }
    }

    private fun writePcm16(buffer: ByteBuffer, encoding: Int, stream: BufferedOutputStream): Long {
        val input = buffer.slice().order(ByteOrder.LITTLE_ENDIAN)
        val output = when (encoding) {
            AudioFormat.ENCODING_PCM_16BIT -> ByteArray(input.remaining()).also { input.get(it) }
            AudioFormat.ENCODING_PCM_FLOAT -> {
                val floats = input.asFloatBuffer()
                ByteBuffer.allocate(floats.remaining() * 2).order(ByteOrder.LITTLE_ENDIAN).also { out ->
                    while (floats.hasRemaining()) out.putShort((floats.get().coerceIn(-1f, 1f) * Short.MAX_VALUE).toInt().toShort())
                }.array()
            }
            AudioFormat.ENCODING_PCM_8BIT -> {
                val out = ByteBuffer.allocate(input.remaining() * 2).order(ByteOrder.LITTLE_ENDIAN)
                while (input.hasRemaining()) out.putShort((((input.get().toInt() and 0xff) - 128) shl 8).toShort())
                out.array()
            }
            else -> throw IllegalArgumentException("Unsupported decoder PCM format")
        }
        stream.write(output)
        return output.size.toLong()
    }

    private fun MediaFormat.intOrDefault(key: String, default: Int): Int =
        if (containsKey(key)) getInteger(key) else default

    private fun writeWavHeader(file: File, dataSize: Long, sampleRate: Int, channels: Int) {
        require(dataSize <= 0xffffffffL - 36) { "Audio file is too large" }
        RandomAccessFile(file, "rw").use { raf ->
            raf.seek(0)
            raf.write("RIFF".toByteArray())
            raf.writeIntLE((36 + dataSize).toInt())
            raf.write("WAVEfmt ".toByteArray())
            raf.writeIntLE(16); raf.writeShortLE(1); raf.writeShortLE(channels)
            raf.writeIntLE(sampleRate); raf.writeIntLE(sampleRate * channels * 2)
            raf.writeShortLE(channels * 2); raf.writeShortLE(16)
            raf.write("data".toByteArray()); raf.writeIntLE(dataSize.toInt())
        }
    }

    private fun RandomAccessFile.writeIntLE(value: Int) { write(value and 0xff); write(value shr 8 and 0xff); write(value shr 16 and 0xff); write(value shr 24 and 0xff) }
    private fun RandomAccessFile.writeShortLE(value: Int) { write(value and 0xff); write(value shr 8 and 0xff) }
}
