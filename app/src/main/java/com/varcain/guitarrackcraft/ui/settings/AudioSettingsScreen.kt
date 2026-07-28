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

package com.varcain.guitarrackcraft.ui.settings

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbManager
import android.os.Build
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.ui.Alignment
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import com.varcain.guitarrackcraft.engine.AudioEngine
import com.varcain.guitarrackcraft.engine.AudioSettingsManager
import com.varcain.guitarrackcraft.engine.DirectUsbAudioManager
import com.varcain.guitarrackcraft.engine.DirectUsbDeviceOption
import com.varcain.guitarrackcraft.engine.DirectUsbFormat
import kotlinx.coroutines.launch

import com.varcain.guitarrackcraft.ui.rack.RackViewModel

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AudioSettingsScreen(
    viewModel: RackViewModel,
    onNavigateBack: () -> Unit
) {
    val context = LocalContext.current
    var selectedBufferSize by remember { mutableIntStateOf(AudioSettingsManager.getBufferSize(context)) }
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
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(horizontal = 16.dp, vertical = 8.dp)
                .verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(20.dp)
        ) {
            DirectUsbSessionSettings()
            BufferSizeDropdown(
                selectedSize = selectedBufferSize,
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
            val isRunning by viewModel.isEngineRunning.collectAsState()
            if (isRunning) {
                Divider()
                Text("Current USB Session", style = MaterialTheme.typography.labelLarge)
                InfoRow("Sample Rate", "%.0f Hz".format(AudioEngine.getSampleRate()))
                InfoRow("Buffer Size", "${AudioEngine.getBufferFrameCount()} frames")
                InfoRow("Input", "USB capture input ${AudioSettingsManager.getDirectUsbInputChannel(context) + 1}")
                InfoRow("Output", "USB outputs ${AudioSettingsManager.getDirectUsbOutputPair(context) * 2 + 1}–${AudioSettingsManager.getDirectUsbOutputPair(context) * 2 + 2}")
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun DirectUsbSessionSettings() {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var devices by remember { mutableStateOf(DirectUsbAudioManager.getAudioDevices(context)) }
    var selectedDevice by remember {
        mutableStateOf(devices.firstOrNull { it.id == AudioSettingsManager.getDirectUsbDeviceId(context) })
    }
    var formats by remember { mutableStateOf<List<DirectUsbFormat>>(emptyList()) }
    var selectedRate by remember { mutableIntStateOf(AudioSettingsManager.getDirectUsbRate(context)) }
    var selectedBits by remember { mutableIntStateOf(AudioSettingsManager.getDirectUsbBits(context)) }
    var selectedInputChannel by remember {
        mutableIntStateOf(AudioSettingsManager.getDirectUsbInputChannel(context))
    }
    var inputChannelCount by remember { mutableIntStateOf(DirectUsbAudioManager.getInputChannelCount()) }
    var selectedOutputPair by remember {
        mutableIntStateOf(AudioSettingsManager.getDirectUsbOutputPair(context))
    }
    var message by remember { mutableStateOf<String?>(null) }
    var devicesExpanded by remember { mutableStateOf(false) }

    fun refreshDevices() {
        val previousDeviceId = selectedDevice?.id
        devices = DirectUsbAudioManager.getAudioDevices(context)
        selectedDevice = devices.firstOrNull { it.id == previousDeviceId }
            ?: devices.firstOrNull()
        if (selectedDevice?.id != previousDeviceId) {
            formats = emptyList()
            inputChannelCount = DirectUsbAudioManager.getInputChannelCount()
        }
        message = null
    }

    DisposableEffect(context) {
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(receiverContext: Context, intent: Intent) {
                if (intent.action == UsbManager.ACTION_USB_DEVICE_ATTACHED ||
                    intent.action == UsbManager.ACTION_USB_DEVICE_DETACHED
                ) {
                    refreshDevices()
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
            it.sampleRate == selectedRate && it.bits == selectedBits
        } ?: formats.firstOrNull()
        if (selected != null) {
            selectedRate = selected.sampleRate

            selectedBits = selected.bits
            DirectUsbAudioManager.startSelected(context, selected)
        }
    }

    fun selectedOutputPairCount(): Int = formats.firstOrNull {
        it.sampleRate == selectedRate && it.bits == selectedBits
    }?.channels?.div(2) ?: 0

    Text("USB Interface", style = MaterialTheme.typography.labelLarge)
    ExposedDropdownMenuBox(
        expanded = devicesExpanded,
        onExpandedChange = { devicesExpanded = it }
    ) {
        OutlinedTextField(
            value = selectedDevice?.name ?: "No USB audio interface found",
            onValueChange = {},
            readOnly = true,
            modifier = Modifier.fillMaxWidth().menuAnchor(),
            trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(devicesExpanded) }
        )
        ExposedDropdownMenu(
            expanded = devicesExpanded,
            onDismissRequest = { devicesExpanded = false }
        ) {
            devices.forEach { device ->
                DropdownMenuItem(
                    text = { Text(device.name) },
                    onClick = {
                        selectedDevice = device
                        formats = emptyList()
                        devicesExpanded = false
                    }
                )
            }
        }
    }
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        OutlinedButton(
            onClick = { refreshDevices() },
            modifier = Modifier.weight(1f)
        ) { Text("Refresh") }
        Button(
            onClick = {
                val device = selectedDevice
                if (device == null) {
                    message = "Connect a USB Audio Class interface first"
                } else {
                    scope.launch {
                        val result = DirectUsbAudioManager.probeFormats(context, device)
                        result.onSuccess { available ->
                            formats = available.sortedWith(
                                compareBy<DirectUsbFormat> { it.sampleRate }.thenBy { it.bits }
                            )
                            val saved = formats.firstOrNull {
                                it.sampleRate == selectedRate && it.bits == selectedBits
                            } ?: formats.firstOrNull()
                            if (saved != null) {
                                selectedRate = saved.sampleRate
                                selectedBits = saved.bits
                                DirectUsbAudioManager.startSelected(context, saved)
                            }
                            inputChannelCount = DirectUsbAudioManager.getInputChannelCount()
                            if (selectedInputChannel >= inputChannelCount) {
                                selectedInputChannel = 0
                                AudioSettingsManager.setDirectUsbInputChannel(context, 0)
                            }
                            if (selectedOutputPair >= selectedOutputPairCount()) {
                                selectedOutputPair = 0
                                AudioSettingsManager.setDirectUsbOutputPair(context, 0)
                            }
                            message = "USB interface configured"
                        }.onFailure { message = it.message }
                    }
                }
            },
            modifier = Modifier.weight(1f)
        ) { Text("Probe formats") }
    }

    val rates = formats.map { it.sampleRate }.distinct()
    val bits = formats.filter { it.sampleRate == selectedRate }.map { it.bits }.distinct()
    if (formats.isNotEmpty()) {
        IntSelector("Sample rate", selectedRate, rates) {
            selectedRate = it
            selectCompatibleFormat()
        }
        IntSelector("Bit depth", selectedBits, bits) {
            selectedBits = it
            selectCompatibleFormat()
        }
        if (inputChannelCount > 0) {
            IntSelector(
                label = "Input",
                selected = selectedInputChannel + 1,
                options = (1..inputChannelCount).toList()
            ) { channel ->
                selectedInputChannel = channel - 1
                AudioSettingsManager.setDirectUsbInputChannel(context, selectedInputChannel)
            }
        }
        val outputPairCount = selectedOutputPairCount()
        if (outputPairCount > 0) {
            IntSelector(
                label = "Output",
                selected = selectedOutputPair + 1,
                options = (1..outputPairCount).toList()
            ) { pair ->
                selectedOutputPair = pair - 1
                AudioSettingsManager.setDirectUsbOutputPair(context, selectedOutputPair)
            }
        }
    }
    message?.let {
        Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun IntSelector(label: String, selected: Int, options: List<Int>, onSelected: (Int) -> Unit) {
    var expanded by remember { mutableStateOf(false) }
    Column {
        Text(label, style = MaterialTheme.typography.labelLarge)
        ExposedDropdownMenuBox(expanded = expanded, onExpandedChange = { expanded = it }) {
            OutlinedTextField(
                value = when (label) {
                    "Sample rate" -> "$selected Hz"
                    "Input" -> "Input $selected"
                    "Output" -> "Outputs ${selected * 2 - 1}–${selected * 2}"
                    else -> "$selected-bit"
                },
                onValueChange = {},
                readOnly = true,
                modifier = Modifier.fillMaxWidth().menuAnchor(),
                trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded) }
            )
            ExposedDropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
                options.forEach { value ->
                    DropdownMenuItem(
                        text = {
                            Text(
                                when (label) {
                                    "Sample rate" -> "$value Hz"
                                    "Input" -> "Input $value"
                                    "Output" -> "Outputs ${value * 2 - 1}–${value * 2}"
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
    onSelected: (Int) -> Unit
) {
    var expanded by remember { mutableStateOf(false) }
    val options = AudioSettingsManager.BUFFER_SIZE_OPTIONS
    val selectedLabel = options.find { it.first == selectedSize }?.second ?: "Auto"

    Column {
        Text(
            text = "Buffer Size",
            style = MaterialTheme.typography.labelLarge,
            modifier = Modifier.padding(bottom = 4.dp)
        )
        ExposedDropdownMenuBox(
            expanded = expanded,
            onExpandedChange = { expanded = it }
        ) {
            OutlinedTextField(
                value = selectedLabel,
                onValueChange = {},
                readOnly = true,
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
                        text = { Text(label) },
                        onClick = {
                            onSelected(size)
                            expanded = false
                        }
                    )
                }
            }
        }
    }
}
