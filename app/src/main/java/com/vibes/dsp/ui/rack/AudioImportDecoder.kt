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
