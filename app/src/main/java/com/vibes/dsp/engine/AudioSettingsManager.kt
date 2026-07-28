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
    private const val KEY_DIRECT_USB_RATE = "directUsbRate"
    private const val KEY_DIRECT_USB_BITS = "directUsbBits"
    private const val KEY_DIRECT_USB_SUBSLOT = "directUsbSubslot"
    private const val KEY_DIRECT_USB_CHANNELS = "directUsbChannels"
    private const val KEY_DIRECT_USB_INPUT_CHANNEL = "directUsbInputChannel"
    private const val KEY_DIRECT_USB_OUTPUT_PAIR = "directUsbOutputPair"

    val BUFFER_SIZE_OPTIONS = listOf(
        0 to "Auto", 16 to "16", 32 to "32", 64 to "64",
        128 to "128", 256 to "256", 512 to "512", 1024 to "1024"
    )

    private fun prefs(context: Context) =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    fun getBufferSize(context: Context): Int = prefs(context).getInt(KEY_BUFFER_SIZE, 0)

    fun setBufferSize(context: Context, size: Int) {
        prefs(context).edit().putInt(KEY_BUFFER_SIZE, size).apply()
    }

    fun getDirectUsbDeviceId(context: Context): Int =
        prefs(context).getInt(KEY_USB_DEVICE_ID, 0)

    fun setDirectUsbDeviceId(context: Context, id: Int) {
        prefs(context).edit().putInt(KEY_USB_DEVICE_ID, id).apply()
    }

    fun getDirectUsbRate(context: Context): Int = prefs(context).getInt(KEY_DIRECT_USB_RATE, 48_000)
    fun getDirectUsbBits(context: Context): Int = prefs(context).getInt(KEY_DIRECT_USB_BITS, 32)
    fun getDirectUsbSubslot(context: Context): Int = prefs(context).getInt(KEY_DIRECT_USB_SUBSLOT, 4)
    fun getDirectUsbChannels(context: Context): Int = prefs(context).getInt(KEY_DIRECT_USB_CHANNELS, 2)
    fun getDirectUsbInputChannel(context: Context): Int =
        prefs(context).getInt(KEY_DIRECT_USB_INPUT_CHANNEL, 0)

    fun setDirectUsbInputChannel(context: Context, channel: Int) {
        prefs(context).edit().putInt(KEY_DIRECT_USB_INPUT_CHANNEL, channel).apply()
    }

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

