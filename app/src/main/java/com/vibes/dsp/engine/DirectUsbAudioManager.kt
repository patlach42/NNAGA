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
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.Job
import kotlinx.coroutines.withContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlin.coroutines.coroutineContext
import kotlin.coroutines.resume
import java.util.concurrent.atomic.AtomicReference

data class DirectUsbDeviceOption(
    val id: Int,
    val name: String,
    val vendorId: Int,
    val productId: Int,
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

data class DirectUsbCalibrationProgress(
    val candidateFrames: Int,
    val candidateIndex: Int,
    val candidateCount: Int,
    val passed: Boolean?,
    val message: String
)

data class DirectUsbCalibrationResult(
    val vendorId: Int,
    val productId: Int,
    val format: DirectUsbFormat,
    val selectedFrames: Int,
    val selectedMilliseconds: Double,
    val passedCandidates: List<Int>,
    val failedCandidates: List<Int>,
    val success: Boolean,
    val message: String
)


/**
 * Owns the app-permitted USB connection used by the direct USB audio session.
 * Native code captures one selected mono channel and sends the processed stereo
 * signal through libusb; Android AudioManager routing is not involved.
 */
object DirectUsbAudioManager {
    private const val TAG = "DirectUsbAudio"
    private const val ACTION_USB_PERMISSION =
        "com.vibes.dsp.action.DIRECT_USB_PERMISSION"
    private const val CALIBRATION_WARMUP_MS = 1_500L
    private const val CALIBRATION_MAX_CANDIDATES = 20
    private const val CALIBRATION_SAFETY_STEPS = 2
    private const val CALIBRATION_MEASURE_MS = 10_000L
    private const val CALIBRATION_CYCLES = 2

    private fun greatestCommonDivisor(left: Int, right: Int): Int {
        var a = left.coerceAtLeast(1)
        var b = right.coerceAtLeast(1)
        while (b != 0) {
            val remainder = a % b
            a = b
            b = remainder
        }
        return a
    }

    private fun calibrationStep(graphFrames: Int, transferFrames: Int): Int {
        val graph = graphFrames.coerceAtLeast(4)
        val transfer = transferFrames.coerceAtLeast(1)
        return greatestCommonDivisor(graph, transfer).coerceAtLeast(16)
    }


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
    private var availableInputChannels = 0
    private val lifecycleMutex = Mutex()
    private val activeCalibrationJob = AtomicReference<Job?>(null)

    fun getAudioDevices(context: Context): List<DirectUsbDeviceOption> {
        val usbManager = context.getSystemService(Context.USB_SERVICE) as UsbManager
        return usbManager.deviceList.values
            .filter(::isUsbAudioDevice)
            .sortedBy { it.deviceName }
            .map { device ->
                DirectUsbDeviceOption(
                    id = device.deviceId,
                    name = buildDeviceName(device),
                    vendorId = device.vendorId,
                    productId = device.productId,
                    device = device
                )
            }
    }

    suspend fun probeFormats(
        context: Context, option: DirectUsbDeviceOption
    ): Result<List<DirectUsbFormat>> = lifecycleMutex.withLock {
        probeFormatsInternal(context, option)
    }

    private suspend fun probeFormatsInternal(
        context: Context, option: DirectUsbDeviceOption
    ): Result<List<DirectUsbFormat>> =
        withContext(Dispatchers.IO) {
            val engine = NativeEngine.getInstance()
            val reusingOpenDevice = activeDeviceId == option.id && connection != null
            if (!reusingOpenDevice) {
                disableInternal(context)
                val usbManager = context.getSystemService(Context.USB_SERVICE) as UsbManager
                if (!requestPermission(context, usbManager, option.device)) {
                    return@withContext Result.failure(SecurityException("USB access was not granted"))
                }
                val opened = usbManager.openDevice(option.device)
                    ?: return@withContext Result.failure(
                        IllegalStateException("Android could not open the USB interface")
                    )
                if (opened.fileDescriptor < 0 || !engine.nativeOpenDirectUsbOutput(opened.fileDescriptor)) {
                    opened.close()
                    return@withContext Result.failure(
                        IllegalStateException(
                            "Could not take control of USB audio. Enable Disable USB audio routing and reconnect the interface."
                        )
                    )
                }
                connection = opened
                activeDeviceId = option.id
                AudioSettingsManager.setDirectUsbDeviceId(context, option.id)
                registerDetachReceiver(context.applicationContext)
            }
            availableInputChannels = engine.nativeGetDirectUsbInputChannelCount()
            if (availableInputChannels <= 0) {
                disableInternal(context)
                return@withContext Result.failure(
                    IllegalStateException("The USB interface exposes no PCM capture channels")
                )
            }
            val packedFormats = runCatching { engine.nativeGetDirectUsbOutputFormats() }
                .getOrDefault(intArrayOf())
            val nativeFormats = packedFormats
                .asSequence()
                .chunked(4)
                .filter { it.size == 4 && it[0] > 0 && it[1] > 0 && it[2] > 0 && it[3] > 0 }
                .map { DirectUsbFormat(it[0], it[1], it[2], it[3]) }
                .toList()
            // Descriptor formats are authoritative; use iD4 fallbacks only when
            // probing returned no usable native descriptors.
            Result.success(if (nativeFormats.isNotEmpty()) nativeFormats else fallbackFormats)
        }

    suspend fun startConfigured(context: Context): Result<Unit> =
        lifecycleMutex.withLock {
            val deviceId = AudioSettingsManager.getDirectUsbDeviceId(context)
            val device = getAudioDevices(context).firstOrNull { it.id == deviceId }
                ?: return@withLock Result.failure(
                    IllegalStateException("No configured USB audio device")
                )
            val selected = DirectUsbFormat(
                AudioSettingsManager.getDirectUsbRate(context),
                AudioSettingsManager.getDirectUsbBits(context),
                AudioSettingsManager.getDirectUsbSubslot(context),
                AudioSettingsManager.getDirectUsbChannels(context)
            )
            val available = probeFormatsInternal(context, device)
                .getOrElse { return@withLock Result.failure(it) }
            val inputChannel = AudioSettingsManager.getDirectUsbInputChannel(context)
            if (inputChannel !in 0 until availableInputChannels) {
                return@withLock Result.failure(
                    IllegalStateException("Configured USB input channel is unavailable")
                )
            }
            val exact = available.firstOrNull {
                it.sampleRate == selected.sampleRate && it.bits == selected.bits &&
                    it.subslotBytes == selected.subslotBytes &&
                    it.channels == selected.channels
            } ?: return@withLock Result.failure(
                IllegalStateException("Configured USB format is unavailable")
            )
            val outputPair = AudioSettingsManager.getDirectUsbOutputPair(context)
            if (outputPair !in 0 until exact.channels / 2) {
                return@withLock Result.failure(
                    IllegalStateException("Configured USB output pair is unavailable")
                )
            }
            val watermark = AudioSettingsManager.getDirectUsbWatermark(
                context, device.vendorId, device.productId, exact.sampleRate,
                exact.bits, exact.subslotBytes, exact.channels
            )
            startExact(context, exact, inputChannel, outputPair, watermark)
        }

    private fun startExact(
        context: Context,
        exact: DirectUsbFormat,
        inputChannel: Int,
        outputPair: Int,
        watermark: Int,
        bufferFrames: Int = AudioSettingsManager.getBufferSize(context),
        periodMultiplier: Int =
            AudioSettingsManager.getDirectUsbPeriodMultiplier(context)
    ): Result<Unit> {
        val engine = NativeEngine.getInstance()
        if (!engine.nativeStartDirectUsbSession(
                exact.sampleRate, exact.bits, exact.subslotBytes, exact.channels,
                inputChannel, outputPair, bufferFrames, periodMultiplier, watermark
            )
        ) {
            disableInternal(context)
            return Result.failure(IllegalStateException("Could not start USB audio session (${exact.label})"))
        }
        return Result.success(Unit)
    }

    suspend fun calibrate(
        context: Context,
        option: DirectUsbDeviceOption,
        format: DirectUsbFormat,
        onProgress: (DirectUsbCalibrationProgress) -> Unit = {}
    ): DirectUsbCalibrationResult {
        val calibrationJob = requireNotNull(coroutineContext[Job])
        check(activeCalibrationJob.compareAndSet(null, calibrationJob)) {
            "USB calibration is already running"
        }
        try {
            return lifecycleMutex.withLock {
                withContext(Dispatchers.IO) {
                    val bufferFrames = AudioSettingsManager.getBufferSize(context)
                    val periodMultiplier =
                        AudioSettingsManager.getDirectUsbPeriodMultiplier(context)
                    val old = AudioSettingsManager.getDirectUsbWatermark(
                        context, option.vendorId, option.productId, format.sampleRate,
                        format.bits, format.subslotBytes, format.channels,
                        bufferFrames, periodMultiplier
                    )
                    val passed = mutableListOf<Int>()
                    val failed = mutableListOf<Int>()
                    try {
                    val available = probeFormatsInternal(context, option).getOrThrow()
                    require(format in available) { "Selected USB format is unavailable" }
                    val inputChannel =
                        AudioSettingsManager.getDirectUsbInputChannel(context)
                            .coerceIn(0, (availableInputChannels - 1).coerceAtLeast(0))
                    val outputPair =
                        AudioSettingsManager.getDirectUsbOutputPair(context)
                            .coerceIn(0, (format.channels / 2 - 1).coerceAtLeast(0))
            startExact(
                context, format, inputChannel, outputPair, 0,
                bufferFrames, periodMultiplier
            ).getOrThrow()
            delay(CALIBRATION_WARMUP_MS)
            val autoStats = NativeEngine.getInstance().getDirectUsbStats()
            val effectiveQuantum = autoStats.effectiveQuantum.toInt()
                .takeIf { it > 0 }
                ?: error("USB driver reported no effective graph quantum")
            val autoTarget = autoStats.steadyTarget.toInt()
                .coerceIn(effectiveQuantum, 4096)
            disableInternal(context)

                    val naturalStep = calibrationStep(
                        effectiveQuantum, autoStats.captureTransferFrames.toInt()
                    )
                    val maxTarget = 4096
                    val grid = buildList {
                        var candidate = effectiveQuantum
                        while (candidate <= maxTarget) {
                            add(candidate)
                            if (candidate > maxTarget - naturalStep) break
                            candidate += naturalStep
                        }
                        if (lastOrNull() != maxTarget) add(maxTarget)
                        add(autoTarget)
                    }.distinct().sorted()
                    val measured = linkedMapOf<Int, Boolean>()

                    suspend fun measureCandidate(candidate: Int): Boolean {
                        measured[candidate]?.let { return it }
                        val index = measured.size
                        var candidatePass = true
                        repeat(CALIBRATION_CYCLES) { cycle ->
                            coroutineContext.ensureActive()
                            withContext(Dispatchers.Main) {
                                onProgress(
                                    DirectUsbCalibrationProgress(
                                        candidate,
                                        index,
                                        CALIBRATION_MAX_CANDIDATES,
                                        null,
                                        "Cycle ${cycle + 1}/$CALIBRATION_CYCLES · warm-up"
                                    )
                                )
                            }
                            val valid = try {
                                probeFormatsInternal(context, option).getOrThrow()
                                startExact(
                                    context, format, inputChannel, outputPair, candidate,
                                    bufferFrames, periodMultiplier
                                ).getOrThrow()
                                delay(CALIBRATION_WARMUP_MS)
                                val before = NativeEngine.getInstance().getDirectUsbStats()
                                delay(CALIBRATION_MEASURE_MS)
                                val after = NativeEngine.getInstance().getDirectUsbStats()
                                val transferErrors =
                                    after.captureTransferErrors - before.captureTransferErrors +
                                        after.playbackTransferErrors -
                                        before.playbackTransferErrors
                                after.schemaVersion >= 5 &&
                                    after.sessionId == before.sessionId &&
                                    after.sequence > before.sequence &&
                                    after.state == DirectUsbSessionState.Running &&
                                    after.failure == DirectUsbFailure.Ok &&
                                    !after.transportFailed &&
                                    after.sampleRateHz == format.sampleRate.toLong() &&
                                    after.effectiveQuantum == effectiveQuantum.toLong() &&
                                    after.periodMultiplier == periodMultiplier.toLong() &&
                                    after.steadyTarget == candidate.toLong() &&
                                    after.queuedOut > 0 &&
                                    after.lastCycleNs > 0 &&
                                    after.deadlineBudgetNs > 0 &&
                                    after.actualXruns - before.actualXruns == 0L &&
                                    after.deadlineMisses - before.deadlineMisses == 0L &&
                                    after.lifecycleFailures -
                                    before.lifecycleFailures == 0L &&
                                    transferErrors == 0L
                            } finally {
                                disableInternal(context)
                            }
                            candidatePass = candidatePass && valid
                            withContext(Dispatchers.Main) {
                                onProgress(
                                    DirectUsbCalibrationProgress(
                                        candidate,
                                        index,
                                        CALIBRATION_MAX_CANDIDATES,
                                        valid,
                                        "Cycle ${cycle + 1}/$CALIBRATION_CYCLES"
                                    )
                                )
                            }
                        }
                        measured[candidate] = candidatePass
                        if (candidatePass) passed += candidate else failed += candidate
                        return candidatePass
                    }

                    val autoIndex = grid.indexOf(autoTarget)
                    var failedIndex = -1
                    var passingIndex = -1
                    if (measureCandidate(autoTarget)) {
                        passingIndex = autoIndex
                    } else {
                        failedIndex = autoIndex
                        while (passingIndex < 0) {
                            val desired = (
                                grid[failedIndex].toLong() * 2L
                            ).coerceAtMost(maxTarget.toLong()).toInt()
                            val probeIndex = ((failedIndex + 1)..grid.lastIndex)
                                .firstOrNull { grid[it] >= desired }
                                ?: grid.lastIndex
                            if (measureCandidate(grid[probeIndex])) {
                                passingIndex = probeIndex
                            } else {
                                failedIndex = probeIndex
                                if (probeIndex == grid.lastIndex) break
                            }
                        }
                    }
                    if (passingIndex < 0) {
                        return@withContext DirectUsbCalibrationResult(
                            option.vendorId, option.productId, format, old,
                            old * 1000.0 / format.sampleRate, passed, failed, false,
                            "No stable watermark candidate"
                        )
                    }
                    while (passingIndex - failedIndex > 1) {
                        val probeIndex = failedIndex + (passingIndex - failedIndex) / 2
                        if (measureCandidate(grid[probeIndex])) {
                            passingIndex = probeIndex
                        } else {
                            failedIndex = probeIndex
                        }
                    }
                    coroutineContext.ensureActive()
                    val reserveTarget = grid[passingIndex].toLong() +
                        naturalStep.toLong() * CALIBRATION_SAFETY_STEPS
                    if (reserveTarget > maxTarget.toLong()) {
                        return@withContext DirectUsbCalibrationResult(
                            option.vendorId, option.productId, format, old,
                            old * 1000.0 / format.sampleRate, passed, failed, false,
                            "No room for the safety reserve below the maximum watermark"
                        )
                    }
                    val firstReserveIndex = ((passingIndex + 1)..grid.lastIndex)
                        .first { grid[it].toLong() >= reserveTarget }
                    var reserveIndex = firstReserveIndex
                    if (!measureCandidate(grid[reserveIndex])) {
                        val knownPassingIndex = (reserveIndex..grid.lastIndex)
                            .firstOrNull { measured[grid[it]] == true }
                        if (knownPassingIndex != null) {
                            reserveIndex = knownPassingIndex
                        } else {
                            var lastFailedIndex = reserveIndex
                            var found = false
                            while (lastFailedIndex < grid.lastIndex) {
                                val distance =
                                    (lastFailedIndex - passingIndex).coerceAtLeast(1)
                                val probeIndex =
                                    (lastFailedIndex + distance * 2)
                                        .coerceAtMost(grid.lastIndex)
                                if (measureCandidate(grid[probeIndex])) {
                                    reserveIndex = probeIndex
                                    found = true
                                    break
                                }
                                lastFailedIndex = probeIndex
                            }
                            if (!found) {
                                return@withContext DirectUsbCalibrationResult(
                                    option.vendorId, option.productId, format, old,
                                    old * 1000.0 / format.sampleRate,
                                    passed, failed, false,
                                    "No stable watermark with the required safety reserve"
                                )
                            }
                        }
                    }
                    coroutineContext.ensureActive()
                    val selected = grid[reserveIndex]
            val persisted = AudioSettingsManager.persistDirectUsbWatermark(
                context, option.vendorId, option.productId, format.sampleRate,
                format.bits, format.subslotBytes, format.channels, selected,
                bufferFrames, periodMultiplier
            )
            if (!persisted) {
                AudioSettingsManager.persistDirectUsbWatermark(
                    context, option.vendorId, option.productId, format.sampleRate,
                    format.bits, format.subslotBytes, format.channels, old,
                    bufferFrames, periodMultiplier
                )
                error("Could not persist the calibrated USB watermark")
            }
            DirectUsbCalibrationResult(
                option.vendorId, option.productId, format, selected,
                selected * 1000.0 / format.sampleRate, passed, failed, true,
                "Calibration complete"
            )
        } catch (cancelled: kotlinx.coroutines.CancellationException) {
            throw cancelled
        } catch (error: Throwable) {
            DirectUsbCalibrationResult(option.vendorId, option.productId, format, old,
                old * 1000.0 / format.sampleRate, passed, failed, false, error.message ?: "Calibration failed")
        } finally {
            disableInternal(context)
        }
                }
            }
        } finally {
            activeCalibrationJob.compareAndSet(calibrationJob, null)
        }
    }

    fun startSelected(context: Context, format: DirectUsbFormat): Result<Unit> {
        AudioSettingsManager.setDirectUsbFormat(
            context, format.sampleRate, format.bits, format.subslotBytes, format.channels
        )
        return Result.success(Unit)
    }

    fun disable(context: Context) {
        val calibration = activeCalibrationJob.get()
        if (calibration != null) {
            calibration.cancel(
                kotlinx.coroutines.CancellationException("USB audio stopped")
            )
            return
        }
        disableInternal(context)
    }

    private fun disableInternal(context: Context) {
        val engine = NativeEngine.getInstance()
        engine.stopEngine()
        engine.nativeStopDirectUsbOutput()
        runCatching { engine.nativeFlushPgoProfile() }
        connection?.close()
        connection = null
        activeDeviceId = null
        availableInputChannels = 0
        unregisterDetachReceiver(context.applicationContext)
    }


    fun isStreaming(): Boolean = NativeEngine.getInstance().nativeIsDirectUsbOutputStreaming()

    fun getInputChannelCount(): Int = availableInputChannels

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
                    runCatching { context.unregisterReceiver(this) }
                    val granted = intent.getBooleanExtra(
                        UsbManager.EXTRA_PERMISSION_GRANTED, false
                    )
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
