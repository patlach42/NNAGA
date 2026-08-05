/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of Guitar RackCraft.
 *
 * Guitar RackCraft is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Guitar RackCraft is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Guitar RackCraft. If not, see <https://www.gnu.org/licenses/>.
 */

package com.vibes.dsp.engine

import android.content.Context

enum class UsbAudioDriver(val code: Int, val persisted: String) {
    Uac(0, "uac"), Line6(1, "line6");
    companion object {
        fun fromPersisted(value: String?): UsbAudioDriver = entries.firstOrNull { it.persisted == value } ?: Uac
    }
}

data class AudioDeviceOption(
    val id: Int,
    val name: String,
    val type: Int
)

data class DirectUsbBufferConfig(
    val playbackTargetFrames: Int,
    val startupPrimeFrames: Int,
    val writeHeadroomFrames: Int,
    val captureLimitFrames: Int,
    val transferCount: Int,
    val packetsPerTransfer: Int,
    val ringCapacityBytes: Int
)

object AudioSettingsManager {
    private const val PREFS_NAME = "audio_settings"
    private const val KEY_USB_DRIVER = "usbAudioDriver"
    private const val KEY_BUFFER_SIZE = "bufferSize"
    private const val KEY_LINE6_SHOW_ALL_USB_DEVICES = "line6ShowAllUsbDevices"
    private const val KEY_USB_DEVICE_ID = "directUsbDeviceId"
    private const val KEY_USB_VENDOR_ID = "directUsbVendorId"
    private const val KEY_USB_PRODUCT_ID = "directUsbProductId"
    private const val KEY_USB_DEVICE_NAME = "directUsbDeviceName"
    private const val KEY_USB_FORMATS = "directUsbFormats"
    private const val KEY_ENGINE_RUN_AT_START = "directUsbEngineRunAtStart"
    private const val KEY_DIRECT_USB_RATE = "directUsbRate"
    private const val KEY_DIRECT_USB_BITS = "directUsbBits"
    private const val KEY_DIRECT_USB_SUBSLOT = "directUsbSubslot"
    private const val KEY_DIRECT_USB_CHANNELS = "directUsbChannels"
    private const val KEY_DIRECT_USB_OUTPUT_PAIR = "directUsbOutputPair"
    private const val KEY_DIRECT_USB_PERIOD_MULTIPLIER = "directUsbPeriodMultiplier"
    private const val KEY_DIRECT_USB_WATERMARK_PREFIX = "directUsbWatermark:"
    private const val KEY_DIRECT_USB_STARTUP_PRIME = "directUsbStartupPrime"
    private const val KEY_DIRECT_USB_WRITE_HEADROOM = "directUsbWriteHeadroom"
    private const val KEY_DIRECT_USB_CAPTURE_LIMIT = "directUsbCaptureLimit"
    private const val KEY_DIRECT_USB_TRANSFER_COUNT = "directUsbTransferCount"
    private const val KEY_DIRECT_USB_PACKETS_PER_TRANSFER = "directUsbPacketsPerTransfer"
    private const val KEY_DIRECT_USB_RING_CAPACITY_KIB = "directUsbRingCapacityKiB"
    private const val KEY_DIRECT_USB_CALIBRATION_PREFIX = "directUsbCalibration:"
    private const val KEY_DIRECT_USB_THERMAL_SAFETY = "directUsbThermalSafety"
    private const val DEFAULT_BUFFER_SIZE = 16

    private const val DEFAULT_DIRECT_USB_PERIOD_MULTIPLIER = 3
    private const val MIN_DIRECT_USB_PERIOD_MULTIPLIER = 1
    private const val MAX_DIRECT_USB_PERIOD_MULTIPLIER = 8
    private const val MAX_DIRECT_USB_WATERMARK = 4096

    private fun clampDirectUsbPeriodMultiplier(value: Int): Int =
        value.coerceIn(MIN_DIRECT_USB_PERIOD_MULTIPLIER, MAX_DIRECT_USB_PERIOD_MULTIPLIER)

    private fun clampWatermark(value: Int): Int = value.coerceIn(0, MAX_DIRECT_USB_WATERMARK)
    fun getDirectUsbStartupPrime(context: Context): Int =
        prefs(context).getInt(KEY_DIRECT_USB_STARTUP_PRIME, 0).coerceIn(0, MAX_DIRECT_USB_WATERMARK)
    fun setDirectUsbStartupPrime(context: Context, frames: Int) {
        prefs(context).edit().putInt(KEY_DIRECT_USB_STARTUP_PRIME, frames.coerceIn(0, MAX_DIRECT_USB_WATERMARK)).apply()
    }
    fun getDirectUsbWriteHeadroom(context: Context): Int =
        prefs(context).getInt(KEY_DIRECT_USB_WRITE_HEADROOM, 0).coerceIn(0, MAX_DIRECT_USB_WATERMARK)
    fun setDirectUsbWriteHeadroom(context: Context, frames: Int) {
        prefs(context).edit().putInt(KEY_DIRECT_USB_WRITE_HEADROOM, frames.coerceIn(0, MAX_DIRECT_USB_WATERMARK)).apply()
    }
    fun getDirectUsbCaptureLimit(context: Context): Int =
        prefs(context).getInt(KEY_DIRECT_USB_CAPTURE_LIMIT, 0).coerceIn(0, MAX_DIRECT_USB_WATERMARK)
    fun setDirectUsbCaptureLimit(context: Context, frames: Int) {
        prefs(context).edit().putInt(KEY_DIRECT_USB_CAPTURE_LIMIT, frames.coerceIn(0, MAX_DIRECT_USB_WATERMARK)).apply()
    }
    fun getDirectUsbTransferCount(context: Context): Int =
        prefs(context).getInt(KEY_DIRECT_USB_TRANSFER_COUNT, 0).coerceIn(0, 8)
    fun setDirectUsbTransferCount(context: Context, count: Int) {
        prefs(context).edit().putInt(KEY_DIRECT_USB_TRANSFER_COUNT, count.coerceIn(0, 8)).apply()
    }
    fun getDirectUsbPacketsPerTransfer(context: Context): Int =
        prefs(context).getInt(KEY_DIRECT_USB_PACKETS_PER_TRANSFER, 0).coerceIn(0, 8)
    fun setDirectUsbPacketsPerTransfer(context: Context, packets: Int) {
        prefs(context).edit().putInt(KEY_DIRECT_USB_PACKETS_PER_TRANSFER, packets.coerceIn(0, 8)).apply()
    }
    fun getDirectUsbRingCapacityKiB(context: Context): Int =
        prefs(context).getInt(KEY_DIRECT_USB_RING_CAPACITY_KIB, 64)
            .takeIf { it == 0 || it in listOf(4, 8, 16, 32, 64, 128, 256, 512, 1024) } ?: 64
    fun setDirectUsbRingCapacityKiB(context: Context, kib: Int) {
        require(kib == 0 || kib in listOf(4, 8, 16, 32, 64, 128, 256, 512, 1024))
        prefs(context).edit().putInt(KEY_DIRECT_USB_RING_CAPACITY_KIB, kib).apply()
    }
    fun getDirectUsbThermalSafetyEnabled(context: Context): Boolean =
        prefs(context).getBoolean(KEY_DIRECT_USB_THERMAL_SAFETY, false)
    fun setDirectUsbThermalSafetyEnabled(context: Context, enabled: Boolean) {
        prefs(context).edit().putBoolean(KEY_DIRECT_USB_THERMAL_SAFETY, enabled).apply()
    }
    fun getDirectUsbBufferConfig(context: Context): DirectUsbBufferConfig =
        DirectUsbBufferConfig(
            playbackTargetFrames = getDirectUsbWatermark(
                context, getDirectUsbVendorId(context), getDirectUsbProductId(context),
                getDirectUsbRate(context), getDirectUsbBits(context),
                getDirectUsbSubslot(context), getDirectUsbChannels(context)
            ),
            startupPrimeFrames = getDirectUsbStartupPrime(context),
            writeHeadroomFrames = getDirectUsbWriteHeadroom(context),
            captureLimitFrames = getDirectUsbCaptureLimit(context),
            transferCount = getDirectUsbTransferCount(context),
            packetsPerTransfer = getDirectUsbPacketsPerTransfer(context),
            ringCapacityBytes = getDirectUsbRingCapacityKiB(context) * 1024
        )
    private fun normalizeBufferSize(value: Int): Int =
        value.takeIf { candidate -> BUFFER_SIZE_OPTIONS.any { it.first == candidate } }
            ?: DEFAULT_BUFFER_SIZE

    private fun watermarkKey(
        vendorId: Int,
        productId: Int,
        rate: Int,
        bits: Int,
        subslotBytes: Int,
        channels: Int,
        bufferFrames: Int,
        periodMultiplier: Int
    ): String =
        "$KEY_DIRECT_USB_WATERMARK_PREFIX$vendorId:$productId:$rate:$bits:" +
            "$subslotBytes:$channels:${normalizeBufferSize(bufferFrames)}:" +
            clampDirectUsbPeriodMultiplier(periodMultiplier)

    fun getDirectUsbPeriodMultiplier(context: Context): Int =
        clampDirectUsbPeriodMultiplier(
            prefs(context).getInt(KEY_DIRECT_USB_PERIOD_MULTIPLIER, DEFAULT_DIRECT_USB_PERIOD_MULTIPLIER)
        )

    fun setDirectUsbPeriodMultiplier(context: Context, multiplier: Int) {
        prefs(context).edit().putInt(KEY_DIRECT_USB_PERIOD_MULTIPLIER, clampDirectUsbPeriodMultiplier(multiplier)).apply()
    }

    fun getDirectUsbWatermark(
        context: Context,
        vendorId: Int,
        productId: Int,
        rate: Int,
        bits: Int,
        subslotBytes: Int,
        channels: Int,
        bufferFrames: Int = getBufferSize(context),
        periodMultiplier: Int = getDirectUsbPeriodMultiplier(context)
    ): Int = clampWatermark(
        prefs(context).getInt(
            watermarkKey(
                vendorId, productId, rate, bits, subslotBytes, channels,
                bufferFrames, periodMultiplier
            ),
            0
        )
    )

    fun setDirectUsbWatermark(
        context: Context,
        vendorId: Int,
        productId: Int,
        rate: Int,
        bits: Int,
        subslotBytes: Int,
        channels: Int,
        frames: Int,
        bufferFrames: Int = getBufferSize(context),
        periodMultiplier: Int = getDirectUsbPeriodMultiplier(context)
    ) {
        prefs(context).edit().putInt(
            watermarkKey(
                vendorId, productId, rate, bits, subslotBytes, channels,
                bufferFrames, periodMultiplier
            ),
            clampWatermark(frames)
        ).apply()
    }
    fun persistDirectUsbWatermark(
        context: Context,
        vendorId: Int,
        productId: Int,
        rate: Int,
        bits: Int,
        subslotBytes: Int,
        channels: Int,
        frames: Int,
        bufferFrames: Int = getBufferSize(context),
        periodMultiplier: Int = getDirectUsbPeriodMultiplier(context)
    ): Boolean = prefs(context).edit().putInt(
        watermarkKey(
            vendorId, productId, rate, bits, subslotBytes, channels,
            bufferFrames, periodMultiplier
        ),
        clampWatermark(frames)
    ).commit()


    val BUFFER_SIZE_OPTIONS = listOf(
        4 to "4 (experimental)", 6 to "6 (experimental)",
        8 to "8 (experimental)", 12 to "12 (experimental)",
        16 to "16", 24 to "24 (experimental)", 32 to "32",
        48 to "48 (experimental)", 64 to "64",
        72 to "72 (experimental)", 96 to "96 (experimental)",
        128 to "128", 256 to "256", 512 to "512", 1024 to "1024"
    )

    private fun prefs(context: Context) =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    fun getBufferSize(context: Context): Int =
        normalizeBufferSize(prefs(context).getInt(KEY_BUFFER_SIZE, DEFAULT_BUFFER_SIZE))

    fun setBufferSize(context: Context, size: Int) {
        prefs(context).edit().putInt(KEY_BUFFER_SIZE, normalizeBufferSize(size)).apply()
    }

    fun getUsbAudioDriver(context: Context): UsbAudioDriver =
        UsbAudioDriver.fromPersisted(prefs(context).getString(KEY_USB_DRIVER, UsbAudioDriver.Uac.persisted))

    fun setUsbAudioDriver(context: Context, driver: UsbAudioDriver) {
        prefs(context).edit().putString(KEY_USB_DRIVER, driver.persisted).apply()
    }

    fun getLine6ShowAllUsbDevices(context: Context): Boolean =
        prefs(context).getBoolean(KEY_LINE6_SHOW_ALL_USB_DEVICES, false)

    fun setLine6ShowAllUsbDevices(context: Context, enabled: Boolean) {
        prefs(context).edit().putBoolean(KEY_LINE6_SHOW_ALL_USB_DEVICES, enabled).apply()
    }

    fun getDirectUsbDeviceId(context: Context): Int =
        prefs(context).getInt(KEY_USB_DEVICE_ID, 0)

    fun setDirectUsbDeviceId(context: Context, id: Int) {
        prefs(context).edit().putInt(KEY_USB_DEVICE_ID, id).apply()
    }

    fun getDirectUsbVendorId(context: Context): Int = prefs(context).getInt(KEY_USB_VENDOR_ID, 0)
    fun getDirectUsbProductId(context: Context): Int = prefs(context).getInt(KEY_USB_PRODUCT_ID, 0)
    fun getDirectUsbDeviceName(context: Context): String =
        prefs(context).getString(KEY_USB_DEVICE_NAME, null).orEmpty()

    fun setDirectUsbIdentity(context: Context, vendorId: Int, productId: Int, name: String) {
        prefs(context).edit()
            .putInt(KEY_USB_VENDOR_ID, vendorId)
            .putInt(KEY_USB_PRODUCT_ID, productId)
            .putString(KEY_USB_DEVICE_NAME, name)
            .apply()
    }

    fun getDirectUsbCachedFormats(context: Context): List<DirectUsbFormat> =
        prefs(context).getString(KEY_USB_FORMATS, null).orEmpty()
            .split(';').mapNotNull { token ->
                val values = token.split(',')
                if (values.size != 4) null else runCatching {
                    DirectUsbFormat(values[0].toInt(), values[1].toInt(), values[2].toInt(), values[3].toInt())
                }.getOrNull()
            }

    fun setDirectUsbCachedFormats(context: Context, formats: List<DirectUsbFormat>) {
        val encoded = formats.joinToString(";") {
            "${it.sampleRate},${it.bits},${it.subslotBytes},${it.channels}"
        }
        prefs(context).edit().putString(KEY_USB_FORMATS, encoded).apply()
    }

    fun getEngineRunAtStart(context: Context): Boolean =
        prefs(context).getBoolean(KEY_ENGINE_RUN_AT_START, false)

    fun setEngineRunAtStart(context: Context, enabled: Boolean) {
        prefs(context).edit().putBoolean(KEY_ENGINE_RUN_AT_START, enabled).apply()
    }

    fun clearDirectUsbSelection(context: Context) {
        listOf(
            KEY_USB_DEVICE_ID, KEY_USB_VENDOR_ID, KEY_USB_PRODUCT_ID, KEY_USB_DEVICE_NAME,
            KEY_USB_FORMATS, KEY_DIRECT_USB_RATE, KEY_DIRECT_USB_BITS,
            KEY_DIRECT_USB_SUBSLOT, KEY_DIRECT_USB_CHANNELS, KEY_DIRECT_USB_OUTPUT_PAIR
        ).fold(prefs(context).edit()) { editor, key -> editor.remove(key) }.apply()
    }

    fun forgetDirectUsbInterface(context: Context) {
        val editor = prefs(context).edit()
        prefs(context).all.keys
            .filter { it.startsWith(KEY_DIRECT_USB_WATERMARK_PREFIX) ||
                it.startsWith(KEY_DIRECT_USB_CALIBRATION_PREFIX) }
            .forEach(editor::remove)
        listOf(
            KEY_USB_DEVICE_ID, KEY_USB_VENDOR_ID, KEY_USB_PRODUCT_ID, KEY_USB_DEVICE_NAME,
            KEY_USB_FORMATS, KEY_ENGINE_RUN_AT_START, KEY_DIRECT_USB_RATE, KEY_DIRECT_USB_BITS,
            KEY_DIRECT_USB_SUBSLOT, KEY_DIRECT_USB_CHANNELS, KEY_DIRECT_USB_OUTPUT_PAIR,
            KEY_DIRECT_USB_PERIOD_MULTIPLIER, KEY_DIRECT_USB_STARTUP_PRIME,
            KEY_DIRECT_USB_WRITE_HEADROOM, KEY_DIRECT_USB_CAPTURE_LIMIT,
            KEY_DIRECT_USB_TRANSFER_COUNT, KEY_DIRECT_USB_PACKETS_PER_TRANSFER,
            KEY_DIRECT_USB_RING_CAPACITY_KIB
        ).forEach(editor::remove)
        editor.apply()
    }

    fun getDirectUsbRate(context: Context): Int = prefs(context).getInt(KEY_DIRECT_USB_RATE, 48_000)

    fun getDirectUsbBits(context: Context): Int = prefs(context).getInt(KEY_DIRECT_USB_BITS, 32)
    fun getDirectUsbSubslot(context: Context): Int = prefs(context).getInt(KEY_DIRECT_USB_SUBSLOT, 4)
    fun getDirectUsbChannels(context: Context): Int = prefs(context).getInt(KEY_DIRECT_USB_CHANNELS, 2)

    fun getDirectUsbOutputPair(context: Context): Int =
        prefs(context).getInt(KEY_DIRECT_USB_OUTPUT_PAIR, 0)

    fun setDirectUsbOutputPair(context: Context, pair: Int) {
        prefs(context).edit().putInt(KEY_DIRECT_USB_OUTPUT_PAIR, pair).apply()
    }

    fun setDirectUsbFormat(context: Context, rate: Int, bits: Int, subslotBytes: Int, channels: Int) {
        prefs(context).edit()
            .putInt(KEY_DIRECT_USB_RATE, rate)
            .putInt(KEY_DIRECT_USB_BITS, bits)
            .putInt(KEY_DIRECT_USB_SUBSLOT, subslotBytes)
            .putInt(KEY_DIRECT_USB_CHANNELS, channels)
            .apply()
    }
    fun persistDirectUsbCalibrationProfile(
        context: Context, vendorId: Int, productId: Int, profile: DirectUsbCalibrationProfile
    ): Boolean {
        val encode: (String) -> String = { it.replace("%", "%25").replace("|", "%7C") }
        val c = profile.bufferConfig
        val value = listOf(
            profile.id, profile.format.sampleRate, profile.format.bits, profile.format.subslotBytes,
            profile.format.channels, profile.bufferFrames, profile.periodMultiplier,
            c.playbackTargetFrames, c.startupPrimeFrames, c.writeHeadroomFrames, c.captureLimitFrames,
            c.transferCount, c.packetsPerTransfer, c.ringCapacityBytes, profile.experimental,
            profile.ranAtEpochMs, profile.started, profile.stable, profile.failure ?: "",
            profile.latencyFrames, profile.latencyMilliseconds, profile.xruns, profile.deadlineMisses,
            profile.transferErrors, profile.deviceMinimumFrames, profile.dangerous,
            profile.extended, profile.label, profile.autoGenerated, profile.attemptedRuns,
            profile.successfulRuns, profile.score
        ).joinToString("|") { encode(it.toString()) }
        return prefs(context).edit().putString(
            "$KEY_DIRECT_USB_CALIBRATION_PREFIX$vendorId:$productId:${profile.id}", value
        ).commit()
    }

    fun getDirectUsbCalibrationProfiles(
        context: Context, vendorId: Int, productId: Int
    ): List<DirectUsbCalibrationProfile> {
        val decode: (String) -> String = { it.replace("%7C", "|").replace("%25", "%") }
        return prefs(context).all.filterKeys {
            it.startsWith("$KEY_DIRECT_USB_CALIBRATION_PREFIX$vendorId:$productId:")
        }.values.mapNotNull { raw ->
            runCatching {
                val v = raw.toString().split('|').map(decode)
                require(v.size == 24 || v.size == 26 || v.size == 28 || v.size == 32)
                DirectUsbCalibrationProfile(v[0],
                    DirectUsbFormat(v[1].toInt(), v[2].toInt(), v[3].toInt(), v[4].toInt()),
                    v[5].toInt(), v[6].toInt(),
                    DirectUsbBufferConfig(v[7].toInt(), v[8].toInt(), v[9].toInt(), v[10].toInt(),
                        v[11].toInt(), v[12].toInt(), v[13].toInt()),
                    v[14].toBoolean(), v[15].toLong(), v[16].toBoolean(), v[17].toBoolean(),
                    v[18].ifEmpty { null }, v[19].toLong(), v[20].toDouble(), v[21].toLong(),
                    v[22].toLong(), v[23].toLong(),
                    if (v.size >= 26) v[24].toInt() else 0,
                    if (v.size >= 26) v[25].toBoolean() else false,
                    if (v.size >= 28) v[26].toBoolean() else false,
                    if (v.size >= 28) v[27] else "",
                    if (v.size >= 32) v[28].toBoolean() else false,
                    if (v.size >= 32) v[29].toInt() else 1,
                    if (v.size >= 32) v[30].toInt() else if (v[16].toBoolean()) 1 else 0,
                    if (v.size >= 32) v[31].toInt() else 0)
            }.getOrNull()
        }.sortedBy { it.id }
    }

    fun applyDirectUsbCalibrationProfile(context: Context, profile: DirectUsbCalibrationProfile) {
        setDirectUsbFormat(context, profile.format.sampleRate, profile.format.bits,
            profile.format.subslotBytes, profile.format.channels)
        setBufferSize(context, profile.bufferFrames)
        setDirectUsbPeriodMultiplier(context, profile.periodMultiplier)
        val c = profile.bufferConfig
        setDirectUsbStartupPrime(context, c.startupPrimeFrames)
        setDirectUsbWriteHeadroom(context, c.writeHeadroomFrames)
        setDirectUsbCaptureLimit(context, c.captureLimitFrames)
        setDirectUsbTransferCount(context, c.transferCount)
        setDirectUsbPacketsPerTransfer(context, c.packetsPerTransfer)
        setDirectUsbRingCapacityKiB(context, c.ringCapacityBytes / 1024)
        setDirectUsbWatermark(context, getDirectUsbVendorId(context), getDirectUsbProductId(context),
            profile.format.sampleRate, profile.format.bits, profile.format.subslotBytes,
            profile.format.channels, c.playbackTargetFrames, profile.bufferFrames, profile.periodMultiplier)
    }

}

