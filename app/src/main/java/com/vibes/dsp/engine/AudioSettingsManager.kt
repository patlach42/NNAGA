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

data class AudioDeviceOption(
    val id: Int,
    val name: String,
    val type: Int
)

object AudioSettingsManager {
    private const val PREFS_NAME = "audio_settings"
    private const val KEY_BUFFER_SIZE = "bufferSize"
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
    private const val DEFAULT_BUFFER_SIZE = 16

    private const val DEFAULT_DIRECT_USB_PERIOD_MULTIPLIER = 3
    private const val MIN_DIRECT_USB_PERIOD_MULTIPLIER = 1
    private const val MAX_DIRECT_USB_PERIOD_MULTIPLIER = 8
    private const val MAX_DIRECT_USB_WATERMARK = 4096

    private fun clampDirectUsbPeriodMultiplier(value: Int): Int =
        value.coerceIn(MIN_DIRECT_USB_PERIOD_MULTIPLIER, MAX_DIRECT_USB_PERIOD_MULTIPLIER)

    private fun clampWatermark(value: Int): Int = value.coerceIn(0, MAX_DIRECT_USB_WATERMARK)
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

    fun forgetDirectUsbInterface(context: Context) {
        val editor = prefs(context).edit()
        prefs(context).all.keys
            .filter { it.startsWith(KEY_DIRECT_USB_WATERMARK_PREFIX) }
            .forEach(editor::remove)
        listOf(
            KEY_USB_DEVICE_ID, KEY_USB_VENDOR_ID, KEY_USB_PRODUCT_ID, KEY_USB_DEVICE_NAME,
            KEY_USB_FORMATS, KEY_ENGINE_RUN_AT_START, KEY_DIRECT_USB_RATE, KEY_DIRECT_USB_BITS,
            KEY_DIRECT_USB_SUBSLOT, KEY_DIRECT_USB_CHANNELS, KEY_DIRECT_USB_OUTPUT_PAIR,
            KEY_DIRECT_USB_PERIOD_MULTIPLIER
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
}

