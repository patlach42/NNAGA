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
package com.vibes.dsp.ui.settings

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbManager
import android.os.Build

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.vibes.dsp.engine.AudioEngine
import com.vibes.dsp.engine.AudioSettingsManager
import com.vibes.dsp.engine.DirectUsbAudioManager
import com.vibes.dsp.engine.DirectUsbDeviceOption
import com.vibes.dsp.engine.DirectUsbFormat
import com.vibes.dsp.engine.UsbAudioDriver
import com.vibes.dsp.ui.rack.RackViewModel
import java.util.Locale
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AudioSettingsScreen(
    viewModel: RackViewModel,
    onNavigateBack: () -> Unit,
    embedded: Boolean = false
) {
    if (embedded) {
        AudioSettingsContent(viewModel = viewModel)
    } else {
        BackHandler { onNavigateBack() }
        Scaffold(
            topBar = {
                TopAppBar(
                    title = { Text("USB Audio Settings") },
                    navigationIcon = {
                        IconButton(onClick = onNavigateBack) {
                            Icon(Icons.Default.ArrowBack, contentDescription = "Back")
                        }
                    }
                )
            }
        ) { padding ->
            AudioSettingsContent(
                viewModel = viewModel,
                modifier = Modifier.padding(padding)
            )
        }
    }
}

@Composable
private fun AudioSettingsContent(
    viewModel: RackViewModel,
    modifier: Modifier = Modifier
) {
    val context = LocalContext.current
    var selectedBufferSize by remember { mutableIntStateOf(AudioSettingsManager.getBufferSize(context)) }
    var isCalibrationRunning by remember { mutableStateOf(false) }
    val isEngineRunning by viewModel.isEngineRunning.collectAsState()
    val directUsbStats by viewModel.directUsbStats.collectAsState()
    DisposableEffect(viewModel) {
        viewModel.setUsbDiagnosticsVisible(true)
        onDispose { viewModel.setUsbDiagnosticsVisible(false) }
    }
    Column(
        modifier = modifier
            .fillMaxSize()
            .padding(horizontal = 12.dp, vertical = 8.dp)
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(16.dp)
    ) {
        DirectUsbSessionSettings(
            selectedBufferFrames = selectedBufferSize,
            inputsEnabled = !isEngineRunning,
            onCalibrationStateChange = { isCalibrationRunning = it }
        )
        BufferSizeDropdown(
            selectedSize = selectedBufferSize,
            enabled = !isCalibrationRunning && !isEngineRunning,
            onSelected = { size ->
                selectedBufferSize = size
                AudioSettingsManager.setBufferSize(context, size)
            }
        )
        Text(
            text = "The rack uses the configured USB audio interface for both input and output. " +
                "Configure the interface before starting a session.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        if (isEngineRunning) {
            Divider()
            Text("Current USB Session", style = MaterialTheme.typography.labelLarge)
            InfoRow("Sample Rate", "%.0f Hz".format(AudioEngine.getSampleRate()))
            InfoRow("Buffer Size", "${AudioEngine.getBufferFrameCount()} frames")
            InfoRow("Output", "USB outputs ${AudioSettingsManager.getDirectUsbOutputPair(context) * 2 + 1}–${AudioSettingsManager.getDirectUsbOutputPair(context) * 2 + 2}")
            val captureContribution = maxOf(
                directUsbStats.effectiveQuantum,
                directUsbStats.captureRingFrames,
                directUsbStats.captureTransferFrames
            )
            val estimatedFrames = directUsbStats.knownHostLatencyFrames
            val estimatedMs = if (directUsbStats.sampleRateHz > 0) {
                estimatedFrames * 1000.0 / directUsbStats.sampleRateHz
            } else {
                0.0
            }
            InfoRow(
                "Estimated host queue",
                "$estimatedFrames frames / ${String.format(Locale.US, "%.2f", estimatedMs)} ms"
            )
            Text(
                text = "max(graph quantum ${directUsbStats.effectiveQuantum}, " +
                    "capture ring ${directUsbStats.captureRingFrames}, " +
                    "capture transfer ${directUsbStats.captureTransferFrames}) = $captureContribution\n" +
                    "+ playback ring ${directUsbStats.playbackRingFrames}\n" +
                    "+ queued OUT ${directUsbStats.queuedOut}\n" +
                    "= $estimatedFrames frames",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            OutlinedButton(
                onClick = viewModel::stopEngine,
                modifier = Modifier.fillMaxWidth()
            ) {
                Text("Stop engine")
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class, ExperimentalFoundationApi::class)
@Composable
private fun DirectUsbSessionSettings(
    selectedBufferFrames: Int,
    inputsEnabled: Boolean,
    onCalibrationStateChange: (Boolean) -> Unit
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val isLine6Driver = AudioSettingsManager.getUsbAudioDriver(context) == UsbAudioDriver.Line6
    var devices by remember { mutableStateOf<List<DirectUsbDeviceOption>>(emptyList()) }
    var selectedDevice by remember { mutableStateOf<DirectUsbDeviceOption?>(null) }
    var formats by remember {
        mutableStateOf(AudioSettingsManager.getDirectUsbCachedFormats(context))
    }
    var selectedRate by remember { mutableIntStateOf(AudioSettingsManager.getDirectUsbRate(context)) }
    var selectedBits by remember { mutableIntStateOf(AudioSettingsManager.getDirectUsbBits(context)) }
    var selectedSubslot by remember { mutableIntStateOf(AudioSettingsManager.getDirectUsbSubslot(context)) }
    var selectedChannels by remember { mutableIntStateOf(AudioSettingsManager.getDirectUsbChannels(context)) }
    var selectedOutputPair by remember {
        mutableIntStateOf(AudioSettingsManager.getDirectUsbOutputPair(context))
    }
    var selectedPeriodMultiplier by remember {
        mutableIntStateOf(AudioSettingsManager.getDirectUsbPeriodMultiplier(context))
    }
    var showAllUsbDevices by remember(isLine6Driver) {
        mutableStateOf(AudioSettingsManager.getLine6ShowAllUsbDevices(context))
    }
    var selectedWatermark by remember { mutableIntStateOf(0) }
    var calibrationProgress by remember { mutableStateOf<com.vibes.dsp.engine.DirectUsbCalibrationProgress?>(null) }
    var calibrationResult by remember { mutableStateOf<com.vibes.dsp.engine.DirectUsbCalibrationResult?>(null) }
    var isCalibrating by remember { mutableStateOf(false) }
    var showUsbInterfaceLog by remember { mutableStateOf(false) }
    var usbInterfaceLog by remember { mutableStateOf("Long press the interface selector to collect diagnostics.") }
    var runAtStart by remember { mutableStateOf(AudioSettingsManager.getEngineRunAtStart(context)) }
    var message by remember { mutableStateOf<String?>(null) }
    val controlsEnabled = inputsEnabled && !isCalibrating
    var devicesExpanded by remember { mutableStateOf(false) }
    val clipboardManager = LocalClipboardManager.current

    fun refreshUsbInterfaceLog() {
        usbInterfaceLog = DirectUsbAudioManager.getAudioDevicesDebugLog(context)
        showUsbInterfaceLog = true
    }

    fun persistFormatSelection(selected: DirectUsbFormat) {
        selectedRate = selected.sampleRate
        selectedBits = selected.bits
        selectedSubslot = selected.subslotBytes
        selectedChannels = selected.channels
        selectedOutputPair = selectedOutputPair.coerceIn(
            0, (selected.channels / 2 - 1).coerceAtLeast(0)
        )
        AudioSettingsManager.setDirectUsbFormat(
            context, selected.sampleRate, selected.bits,
            selected.subslotBytes, selected.channels
        )
        AudioSettingsManager.setDirectUsbOutputPair(context, selectedOutputPair)
    }

    fun reconcileFormatSelection(): DirectUsbFormat? {
        val selected = formats.firstOrNull {
            it.sampleRate == selectedRate && it.bits == selectedBits &&
                it.subslotBytes == selectedSubslot && it.channels == selectedChannels
        } ?: formats.firstOrNull {
            it.sampleRate == selectedRate
        } ?: formats.firstOrNull()
        selected?.let { persistFormatSelection(it) }
        return selected
    }

    suspend fun refreshDevices() {
        val preferredDeviceId = AudioSettingsManager.getDirectUsbDeviceId(context)
        val preferredVendor = AudioSettingsManager.getDirectUsbVendorId(context)
        val preferredProduct = AudioSettingsManager.getDirectUsbProductId(context)
        val refreshedDevices = withContext(Dispatchers.IO) {
            DirectUsbAudioManager.getAudioDevices(context)
        }
        devices = refreshedDevices
        selectedDevice = refreshedDevices.firstOrNull { it.id == preferredDeviceId }
            ?: refreshedDevices.firstOrNull {
                it.vendorId == preferredVendor && it.productId == preferredProduct
            }
        formats = AudioSettingsManager.getDirectUsbCachedFormats(context)
        reconcileFormatSelection()
        message = null

        val device = selectedDevice
        if (device != null && inputsEnabled) {
            DirectUsbAudioManager.probeFormats(context, device)
                .onSuccess { available ->
                    formats = available.sortedWith(
                        compareBy<DirectUsbFormat> { it.sampleRate }.thenBy { it.bits }
                    )
                    reconcileFormatSelection()
                }
                .onFailure { error ->
                    if (formats.isEmpty()) message = error.message
                }
        }
    }

    LaunchedEffect(context, inputsEnabled) {
        refreshDevices()
    }

    DisposableEffect(context) {
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(receiverContext: Context, intent: Intent) {
                if (intent.action == UsbManager.ACTION_USB_DEVICE_ATTACHED ||
                    intent.action == UsbManager.ACTION_USB_DEVICE_DETACHED
                ) {
                    scope.launch { refreshDevices() }
                }
            }
        }
        val filter = IntentFilter().apply {
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            context.registerReceiver(receiver, filter, Context.RECEIVER_EXPORTED)
        } else {
            @Suppress("UnspecifiedRegisterReceiverFlag")
            context.registerReceiver(receiver, filter)
        }
        onDispose { runCatching { context.unregisterReceiver(receiver) } }
    }

    fun selectCompatibleFormat() {
        val selected = formats.firstOrNull {
            it.sampleRate == selectedRate && it.bits == selectedBits &&
                it.subslotBytes == selectedSubslot && it.channels == selectedChannels
        } ?: formats.firstOrNull {
            it.sampleRate == selectedRate && it.bits == selectedBits
        } ?: formats.firstOrNull {
            it.sampleRate == selectedRate
        } ?: formats.firstOrNull()
        if (selected != null) {
            persistFormatSelection(selected)
            DirectUsbAudioManager.startSelected(context, selected)
        }
    }

    fun selectedOutputPairCount(): Int = formats.firstOrNull {
        it.sampleRate == selectedRate && it.bits == selectedBits &&
            it.subslotBytes == selectedSubslot && it.channels == selectedChannels
    }?.channels?.div(2) ?: 0

    fun loadDevice(device: DirectUsbDeviceOption) {
        selectedDevice = device
        devicesExpanded = false
        scope.launch {
            DirectUsbAudioManager.probeFormats(context, device).onSuccess { available ->
                formats = available.sortedWith(compareBy<DirectUsbFormat> { it.sampleRate }.thenBy { it.bits })
                val selected = reconcileFormatSelection()
                if (selected != null) {
                    DirectUsbAudioManager.startSelected(context, selected)
                }
                message = "USB interface configured"
            }.onFailure { message = it.message }
        }
    }

    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        Text("USB Interface", style = MaterialTheme.typography.labelLarge)
        IconButton(
            onClick = { scope.launch { refreshDevices() } },
            enabled = controlsEnabled
        ) { Icon(Icons.Default.Refresh, contentDescription = "Refresh USB interfaces") }
    }
    val renderInterfaceSelector: @Composable () -> Unit = {
        ExposedDropdownMenuBox(
            expanded = devicesExpanded,
            onExpandedChange = { if (controlsEnabled) devicesExpanded = it }
        ) {
            Box(modifier = Modifier.fillMaxWidth()) {
                OutlinedTextField(
                    value = selectedDevice?.name
                        ?: AudioSettingsManager.getDirectUsbDeviceName(context)
                            .ifEmpty { "No USB audio interface found" } +
                        if (selectedDevice == null &&
                            AudioSettingsManager.getDirectUsbDeviceName(context).isNotEmpty()
                        ) " (disconnected)" else "",
                    onValueChange = {},
                    readOnly = true,
                    enabled = controlsEnabled,
                    modifier = Modifier
                        .fillMaxWidth()
                        .menuAnchor(),
                    trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(devicesExpanded) }
                )
                Box(
                    modifier = Modifier
                        .matchParentSize()
                        .combinedClickable(
                            enabled = controlsEnabled,
                            onClick = { devicesExpanded = !devicesExpanded },
                            onLongClick = { refreshUsbInterfaceLog() }
                        )
                )
            }
            ExposedDropdownMenu(
                expanded = devicesExpanded,
                onDismissRequest = { devicesExpanded = false }
            ) {
                devices.forEach { device ->
                    DropdownMenuItem(
                        text = { Text(device.name) },
                        enabled = controlsEnabled,
                        onClick = { loadDevice(device) }
                    )
                }
            }
        }
    }
    if (isLine6Driver) {
        Column(modifier = Modifier.fillMaxWidth()) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Text("Show all USB devices")
                Switch(
                    checked = showAllUsbDevices,
                    enabled = controlsEnabled,
                    onCheckedChange = {
                        showAllUsbDevices = it
                        AudioSettingsManager.setLine6ShowAllUsbDevices(context, it)
                        scope.launch { refreshDevices() }
                    }
                )
            }
            renderInterfaceSelector()
        }
    } else {
        renderInterfaceSelector()
    }

    if (showUsbInterfaceLog) {
        val logScrollState = rememberScrollState()
        AlertDialog(
            onDismissRequest = { showUsbInterfaceLog = false },
            title = { Text("USB interface scan log") },
            text = {
                Text(
                    usbInterfaceLog,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier
                        .fillMaxWidth()
                        .heightIn(max = 320.dp)
                        .verticalScroll(logScrollState)
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    clipboardManager.setText(AnnotatedString(usbInterfaceLog))
                    showUsbInterfaceLog = false
                }) {
                    Text("Copy")
                }
            },
            dismissButton = {
                TextButton(onClick = { showUsbInterfaceLog = false }) {
                    Text("Close")
                }
            }
        )
    }

    val rates = formats.map { it.sampleRate }.distinct()
    val bits = formats.filter { it.sampleRate == selectedRate }.map { it.bits }.distinct()
    if (formats.isNotEmpty()) {
        IntSelector("Sample rate", selectedRate, rates, enabled = controlsEnabled) {
            selectedRate = it
            selectCompatibleFormat()
        }
        IntSelector("Bit depth", selectedBits, bits, enabled = controlsEnabled) {
            selectedBits = it
            selectCompatibleFormat()
        }
        val outputPairCount = selectedOutputPairCount()
        if (outputPairCount > 0) {
            IntSelector(
                label = "Output",
                selected = selectedOutputPair + 1,
                options = (1..outputPairCount).toList(),
                enabled = controlsEnabled,
            ) { pair ->
                selectedOutputPair = pair - 1
                AudioSettingsManager.setDirectUsbOutputPair(context, selectedOutputPair)
            }
        }
        val selectedFormat = formats.firstOrNull {
            it.sampleRate == selectedRate && it.bits == selectedBits &&
                it.subslotBytes == selectedSubslot && it.channels == selectedChannels
        }
        LaunchedEffect(
            selectedDevice?.vendorId,
            selectedDevice?.productId,
            selectedFormat,
            selectedBufferFrames,
            selectedPeriodMultiplier
        ) {
            selectedWatermark = if (selectedDevice != null && selectedFormat != null) {
                AudioSettingsManager.getDirectUsbWatermark(
                    context,
                    selectedDevice!!.vendorId,
                    selectedDevice!!.productId,
                    selectedFormat.sampleRate,
                    selectedFormat.bits,
                    selectedFormat.subslotBytes,
                    selectedFormat.channels,
                    selectedBufferFrames,
                    selectedPeriodMultiplier
                )
            } else {
                0
            }
        }
        val watermarkOptions = remember(selectedWatermark) {
            (listOf(
                0, 16, 32, 48, 64, 96, 128, 144, 192, 256, 384, 512,
                768, 1024, 1536, 2048, 3072, 4096, selectedWatermark
            )).distinct().sorted()
        }
        IntSelector(
            label = "Userspace watermark",
            selected = selectedWatermark,
            options = watermarkOptions,
            enabled = controlsEnabled,
        ) { frames ->
            selectedWatermark = frames
            val device = selectedDevice
            val format = selectedFormat
            if (device != null && format != null) {
                AudioSettingsManager.setDirectUsbWatermark(
                    context,
                    device.vendorId,
                    device.productId,
                    format.sampleRate,
                    format.bits,
                    format.subslotBytes,
                    format.channels,
                    frames,
                    selectedBufferFrames,
                    selectedPeriodMultiplier
                )
            }
        }
        Text(
            "Auto derives a safe transport target; manual watermark can only raise that " +
                "Auto floor. Period multiplier affects Auto only.",
        )
        selectedFormat?.let { format ->
            val device = selectedDevice
            OutlinedButton(
                onClick = {
                    if (device == null) {
                        message = "Select an interface first"
                    } else {
                        scope.launch {
                            isCalibrating = true
                            onCalibrationStateChange(true)
                            calibrationResult = null
                            calibrationProgress = null
                            try {
                                DirectUsbAudioManager.calibrate(context, device, format) {
                                    calibrationProgress = it
                                }.also { result ->
                                    calibrationResult = result
                                    if (selectedDevice?.id == device.id &&
                                        selectedFormat == result.format
                                    ) {
                                        selectedWatermark = result.selectedFrames
                                    }
                                }
                            } finally {
                                isCalibrating = false
                                onCalibrationStateChange(false)
                            }
                        }
                    }
                },
                enabled = device != null && controlsEnabled
            ) {
                Text(if (isCalibrating) "Calibrating…" else "Calibrate this interface")
            }
            Text(
                "Stops the engine. Two measured runs per target; allow about 2–9 minutes.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            calibrationProgress?.let {
                Text("Candidate ${it.candidateIndex + 1}/${it.candidateCount}: ${it.candidateFrames} frames — ${it.message}")
            }
            calibrationResult?.let {
                val vidPid = "%04x:%04x".format(it.vendorId, it.productId)
                val milliseconds = "%.2f".format(it.selectedMilliseconds)
                val passedVariants = it.passedCandidates
                    .joinToString(separator = ", ", postfix = " frames")
                    .ifEmpty { "none" }
                Text(
                    "VID:PID $vidPid · ${it.format.sampleRate} Hz/${it.format.bits}-bit/" +
                        "${it.format.subslotBytes}-byte/${it.format.channels}ch · " +
                        "${it.message}: ${it.selectedFrames} frames ($milliseconds ms); " +
                        "passed watermark variants: $passedVariants; " +
                        "failed ${it.failedCandidates.joinToString()}",
                    style = MaterialTheme.typography.bodySmall
                )
            }
        }
    }

    IntSelector(
        label = "Period multiplier",
        selected = selectedPeriodMultiplier,
        options = (1..8).toList(),
        enabled = controlsEnabled,
    ) { multiplier ->
        selectedPeriodMultiplier = multiplier
        AudioSettingsManager.setDirectUsbPeriodMultiplier(context, multiplier)
    }
    Text(
        text = "Lower values reduce the Auto reserve and latency but increase the risk of audio " +
            "dropouts. Changes apply on the next engine start.",
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant
    )
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        Text("Engine run at app start")
        Switch(
            checked = runAtStart,
            onCheckedChange = {
                runAtStart = it
                AudioSettingsManager.setEngineRunAtStart(context, it)
            },
            enabled = controlsEnabled
        )
    }
    OutlinedButton(
        onClick = {
            DirectUsbAudioManager.disable(context)
            AudioSettingsManager.forgetDirectUsbInterface(context)
            selectedDevice = null
            formats = emptyList()
            runAtStart = false
            message = "Forgot USB interface settings"
        },
        enabled = controlsEnabled
    ) { Text("Forget USB interface") }
    message?.let {
        Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun IntSelector(
    label: String,
    selected: Int,
    options: List<Int>,
    enabled: Boolean = true,
    onSelected: (Int) -> Unit
) {
    var expanded by remember { mutableStateOf(false) }
    Column {
        Text(label, style = MaterialTheme.typography.labelLarge)
        ExposedDropdownMenuBox(
            expanded = expanded,
            onExpandedChange = { if (enabled) expanded = it }
        ) {
            OutlinedTextField(
                value = when (label) {
                    "Sample rate" -> "$selected Hz"
                    "Input" -> "Input $selected"
                    "Output" -> "Outputs ${selected * 2 - 1}–${selected * 2}"
                    "Period multiplier" -> "$selected×"
                    "Userspace watermark" -> if (selected == 0) "Auto" else "$selected frames"
                    else -> "$selected-bit"
                },
                onValueChange = {},
                readOnly = true,
                enabled = enabled,
                modifier = Modifier.fillMaxWidth().menuAnchor(),
                trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded) }
            )
            ExposedDropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
                options.forEach { value ->
                    DropdownMenuItem(
                        enabled = enabled,
                        text = {
                            Text(
                                when (label) {
                                    "Sample rate" -> "$value Hz"
                                    "Input" -> "Input $value"
                                    "Output" -> "Outputs ${value * 2 - 1}–${value * 2}"
                                    "Period multiplier" -> "$value×"
                                    "Userspace watermark" -> if (value == 0) "Auto" else "$value frames"
                                    else -> "$value-bit"
                                }
                            )
                        },
                        onClick = {
                            onSelected(value)
                            expanded = false
                        }
                    )
                }
            }
        }
    }
}


@Composable
private fun InfoRow(label: String, value: String) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp),
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Text(
            text = value,
            style = MaterialTheme.typography.bodyMedium
        )
    }
}


@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun BufferSizeDropdown(
    selectedSize: Int,
    enabled: Boolean,
    onSelected: (Int) -> Unit
) {
    var expanded by remember { mutableStateOf(false) }
    val options = AudioSettingsManager.BUFFER_SIZE_OPTIONS
    val selectedLabel = options.find { it.first == selectedSize }?.second ?: "16"

    Column {
        Text(
            text = "Buffer Size",
            style = MaterialTheme.typography.labelLarge,
            modifier = Modifier.padding(bottom = 4.dp)
        )
        ExposedDropdownMenuBox(
            expanded = expanded,
            onExpandedChange = { if (enabled) expanded = it }
        ) {
            OutlinedTextField(
                value = selectedLabel,
                onValueChange = {},
                readOnly = true,
                enabled = enabled,
                modifier = Modifier
                    .fillMaxWidth()
                    .menuAnchor(),
                trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = expanded) }
            )
            ExposedDropdownMenu(
                expanded = expanded,
                onDismissRequest = { expanded = false }
            ) {
                options.forEach { (size, label) ->
                    DropdownMenuItem(
                        enabled = enabled,
                        text = { Text(label) },
                        onClick = {
                            onSelected(size)
                            expanded = false
                        }
                    )
                }
            }
        }
        Text(
            "Experimental values can reduce latency but may underrun with heavy plug-ins.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
    }
}
