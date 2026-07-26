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

package com.varcain.guitarrackcraft.engine

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.os.Build
import android.util.Log
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlin.coroutines.resume

data class DirectUsbDeviceOption(
    val id: Int,
    val name: String,
    internal val device: UsbDevice
)

data class DirectUsbFormat(
    val sampleRate: Int,
    val bits: Int,
    val subslotBytes: Int,
    val channels: Int = 2
) {
    val label: String
        get() = "${sampleRate} Hz · $bits-bit" +
            if (channels == 2) "" else " · $channels channels"
}

/**
 * Owns the Android USB permission and connection required by the native UAC
 * playback prototype. The direct path is intentionally output-only: guitar
 * input remains on the existing Oboe input stream until UAC capture exists.
 */
object DirectUsbAudioManager {
    private const val TAG = "DirectUsbAudio"
    private const val ACTION_USB_PERMISSION =
        "com.varcain.guitarrackcraft.action.DIRECT_USB_PERMISSION"


    private val fallbackFormats = listOf(
        DirectUsbFormat(44_100, 16, 2),
        DirectUsbFormat(44_100, 24, 3),
        DirectUsbFormat(48_000, 16, 2),
        DirectUsbFormat(48_000, 24, 3),
        DirectUsbFormat(48_000, 24, 4, 4),
        DirectUsbFormat(48_000, 32, 4)
    )
    private var connection: UsbDeviceConnection? = null
    private var activeDeviceId: Int? = null
    private var detachReceiver: BroadcastReceiver? = null

    fun getAudioDevices(context: Context): List<DirectUsbDeviceOption> {
        val usbManager = context.getSystemService(Context.USB_SERVICE) as UsbManager
        return usbManager.deviceList.values
            .filter(::isUsbAudioDevice)
            .sortedBy { it.deviceName }
            .map { device ->
                DirectUsbDeviceOption(
                    id = device.deviceId,
                    name = buildDeviceName(device),
                    device = device
                )
            }
    }

    suspend fun probeFormats(context: Context, option: DirectUsbDeviceOption): Result<List<DirectUsbFormat>> {
        disable(context)
        val usbManager = context.getSystemService(Context.USB_SERVICE) as UsbManager
        if (!requestPermission(context, usbManager, option.device)) {
            return Result.failure(SecurityException("USB access was not granted"))
        }
        val opened = usbManager.openDevice(option.device)
            ?: return Result.failure(IllegalStateException("Android could not open the USB interface"))
        val engine = NativeEngine.getInstance()
        if (opened.fileDescriptor < 0 || !engine.nativeOpenDirectUsbOutput(opened.fileDescriptor)) {
            opened.close()
            return Result.failure(IllegalStateException("Could not take control of USB audio. Enable Disable USB audio routing and reconnect the interface."))
        }
        connection = opened
        activeDeviceId = option.id
        registerDetachReceiver(context.applicationContext)
        val packedFormats = runCatching { engine.nativeGetDirectUsbOutputFormats() }.getOrDefault(intArrayOf())
        val nativeFormats = packedFormats
            .asSequence()
            .chunked(4)
            .filter { it.size == 4 && it[0] > 0 && it[1] > 0 && it[2] > 0 && it[3] > 0 }
            .map { DirectUsbFormat(it[0], it[1], it[2], it[3]) }
            .toList()
        // Keep the known iD4 UAC2 alternate available when descriptor probing is incomplete.
        return Result.success((nativeFormats + fallbackFormats).distinct())
    }

    fun startSelected(context: Context, format: DirectUsbFormat): Result<Unit> {
        if (AudioEngine.getSampleRate().toInt() != format.sampleRate) {
            return Result.failure(IllegalStateException("Engine is not running at ${format.sampleRate} Hz"))
        }
        val engine = NativeEngine.getInstance()
        if (!engine.nativeStartDirectUsbOutput(
                format.sampleRate,
                format.bits,
                format.subslotBytes,
                format.channels
            )
        ) {
            return Result.failure(IllegalStateException("Could not start ${format.label} playback"))
        }
        AudioSettingsManager.setDirectUsbFormat(
            context,
            format.sampleRate,
            format.bits,
            format.subslotBytes,
            format.channels
        )
        return Result.success(Unit)
    }


    fun disable(context: Context) {
        NativeEngine.getInstance().nativeStopDirectUsbOutput()
        connection?.close()
        connection = null
        activeDeviceId = null
        unregisterDetachReceiver(context.applicationContext)
    }

    fun isStreaming(): Boolean = NativeEngine.getInstance().nativeIsDirectUsbOutputStreaming()

    private fun isUsbAudioDevice(device: UsbDevice): Boolean =
        device.deviceClass == UsbConstants.USB_CLASS_AUDIO ||
            (0 until device.interfaceCount).any { index ->
                device.getInterface(index).interfaceClass == UsbConstants.USB_CLASS_AUDIO
            }

    private fun buildDeviceName(device: UsbDevice): String {
        val manufacturer = device.manufacturerName?.takeIf(String::isNotBlank)
        val product = device.productName?.takeIf(String::isNotBlank)
        return listOfNotNull(manufacturer, product).joinToString(" ").ifBlank {
            "USB audio device ${device.vendorId.toString(16)}:${device.productId.toString(16)}"
        }
    }

    private suspend fun requestPermission(
        context: Context,
        usbManager: UsbManager,
        device: UsbDevice
    ): Boolean {
        if (usbManager.hasPermission(device)) return true
        return suspendCancellableCoroutine { continuation ->
            val receiver = object : BroadcastReceiver() {
                override fun onReceive(receiverContext: Context, intent: Intent) {
                    if (intent.action != ACTION_USB_PERMISSION) return
                    val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                    runCatching { context.unregisterReceiver(this) }
                    if (continuation.isActive) continuation.resume(granted)
                }
            }
            val filter = IntentFilter(ACTION_USB_PERMISSION)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                context.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED)
            } else {
                @Suppress("UnspecifiedRegisterReceiverFlag")
                context.registerReceiver(receiver, filter)
            }
            continuation.invokeOnCancellation { runCatching { context.unregisterReceiver(receiver) } }
            val flags = PendingIntent.FLAG_UPDATE_CURRENT or
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) PendingIntent.FLAG_MUTABLE else 0
            val intent = Intent(ACTION_USB_PERMISSION).setPackage(context.packageName)
            usbManager.requestPermission(device, PendingIntent.getBroadcast(context, 0, intent, flags))
        }
    }

    private fun registerDetachReceiver(context: Context) {
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(receiverContext: Context, intent: Intent) {
                if (intent.action != UsbManager.ACTION_USB_DEVICE_DETACHED) return
                val device = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
                } else {
                    @Suppress("DEPRECATION")
                    intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
                }
                if (device?.deviceId == activeDeviceId) {
                    Log.i(TAG, "Direct USB device detached")
                    disable(context)
                }
            }
        }
        val filter = IntentFilter(UsbManager.ACTION_USB_DEVICE_DETACHED)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            context.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("UnspecifiedRegisterReceiverFlag")
            context.registerReceiver(receiver, filter)
        }
        detachReceiver = receiver
    }

    private fun unregisterDetachReceiver(context: Context) {
        detachReceiver?.let { receiver -> runCatching { context.unregisterReceiver(receiver) } }
        detachReceiver = null
    }
}
