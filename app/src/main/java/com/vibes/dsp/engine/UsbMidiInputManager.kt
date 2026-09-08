package com.vibes.dsp.engine

import android.content.Context
import android.content.pm.PackageManager
import android.hardware.usb.UsbDevice
import android.media.midi.MidiDevice
import android.media.midi.MidiDeviceInfo
import android.media.midi.MidiManager
import android.media.midi.MidiOutputPort
import android.media.midi.MidiReceiver
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.util.IdentityHashMap
import java.util.concurrent.Executor
import java.util.concurrent.atomic.AtomicBoolean

/** Persisted identity. [MidiDeviceInfo.id] is deliberately session-only. */
data class UsbMidiPortIdentity(
    val vendorId: Int,
    val productId: Int,
    val serialNumber: String,
    val portNumber: Int,
)

data class UsbMidiEndpoint(
    val identity: UsbMidiPortIdentity,
    val displayName: String,
    val deviceToken: Int,
)

enum class MidiAssignmentStatus {
    Disconnected,
    Ambiguous,
    Opening,
    Connected,
    Failed,
}

data class UsbMidiAssignment(
    val identity: UsbMidiPortIdentity,
    val displayName: String,
    val deviceToken: Int? = null,
    val status: MidiAssignmentStatus = MidiAssignmentStatus.Disconnected,
    val sourceHandle: Long = 0L,
)

enum class MidiAvailability { Unsupported, Available }

data class UsbMidiState(
    val availability: MidiAvailability = MidiAvailability.Unsupported,
    val endpoints: List<UsbMidiEndpoint> = emptyList(),
    val assignments: Map<Long, UsbMidiAssignment> = emptyMap(),
    val sessionGeneration: Long = 0L,
)

internal interface MidiIngressSink {
    fun register(identity: UsbMidiPortIdentity): Long
    fun unregister(handle: Long)
    fun flush(handle: Long)
    fun bind(trackId: Long, handle: Long): Boolean
    fun enqueue(
        handle: Long,
        timestamps: LongArray,
        offsets: IntArray,
        lengths: IntArray,
        payload: ByteArray,
        count: Int,
        oversizeDelta: Int,
        malformedDelta: Int,
    ): Int
}

/** Android-free platform boundary. Callbacks are serialized on its control executor. */
internal interface MidiPlatform {
    fun enumerate(): List<UsbMidiEndpoint>
    fun startDiscovery(onChanged: (List<UsbMidiEndpoint>) -> Unit) {
        onChanged(enumerate())
    }
    fun stopDiscovery() = Unit
    fun open(token: Int, callback: (MidiPlatformDevice?) -> Unit) {
        callback(null)
    }
    fun postControl(block: () -> Unit) = block()
}

internal interface MidiPlatformDevice {
    val token: Int
    fun openOutputPort(portNumber: Int): MidiPlatformPort?
    fun close()
}

internal interface MidiPlatformPort {
    val portNumber: Int
    fun connect(receiver: MidiPlatformReceiver): Boolean
    fun disconnect(receiver: MidiPlatformReceiver)
    fun close()
}

internal interface MidiPlatformReceiver {
    fun onSend(data: ByteArray, offset: Int, count: Int, timestampNanos: Long)
    fun onFlush()
}

/** MIDI 1.0 byte-stream parser with fixed storage retained for one open port. */
internal class MidiByteStreamParser(
    private val sink: BatchSink,
    private val clock: () -> Long = System::nanoTime,
) {
    internal interface BatchSink {
        fun submit(
            timestamps: LongArray,
            offsets: IntArray,
            lengths: IntArray,
            payload: ByteArray,
            count: Int,
            oversize: Int,
            malformed: Int,
        )
    }

    companion object {
        const val MAX_PAYLOAD = 65_536
        const val MAX_EVENTS = 128
    }

    private val sysex = ByteArray(MAX_PAYLOAD)
    private val batch = ByteArray(MAX_PAYLOAD)
    private val timestamps = LongArray(MAX_EVENTS)
    private val offsets = IntArray(MAX_EVENTS)
    private val lengths = IntArray(MAX_EVENTS)
    private val shortMessage = ByteArray(3)
    private val realtimeMessage = ByteArray(1)

    private var batchCount = 0
    private var batchBytes = 0
    private var runningStatus = 0
    private var pendingStatus = 0
    private var pendingDataBytes = 0
    private var pendingDataCount = 0
    private var messageTimestamp = 0L
    private var inSysex = false
    private var droppingOversizeSysex = false
    private var sysexBytes = 0
    private var oversizeDelta = 0
    private var malformedDelta = 0

    fun onBytes(data: ByteArray, offset: Int, length: Int, callbackTimestamp: Long) {
        val now = clock()
        val timestamp = if (callbackTimestamp <= 0L || callbackTimestamp > now) {
            now
        } else {
            callbackTimestamp
        }
        if (offset < 0 || length < 0 || offset > data.size || length > data.size - offset) {
            malformedDelta++
            flush()
            return
        }

        var index = offset
        val end = offset + length
        while (index < end) {
            val value = data[index].toInt() and 0xff
            when {
                value >= 0xf8 -> emitRealtime(value, timestamp)
                inSysex -> consumeSysexByte(value, timestamp)
                value == 0xf0 -> beginSysex(timestamp)
                value >= 0x80 -> beginStatus(value, timestamp)
                else -> consumeDataByte(value, timestamp)
            }
            index++
        }
        flush()
    }

    private fun emitRealtime(status: Int, timestamp: Long) {
        realtimeMessage[0] = status.toByte()
        append(realtimeMessage, 0, 1, timestamp)
    }

    private fun consumeSysexByte(value: Int, timestamp: Long) {
        if (droppingOversizeSysex) {
            if (value == 0xf7) resetSysex()
            return
        }
        when {
            value == 0xf7 -> {
                sysex[sysexBytes++] = value.toByte()
                append(sysex, 0, sysexBytes, messageTimestamp)
                resetSysex()
            }
            value == 0xf0 -> {
                malformedDelta++
                beginSysex(timestamp, countInterrupted = false)
            }
            value >= 0x80 -> {
                malformedDelta++
                resetSysex()
                beginStatus(value, timestamp)
            }
            sysexBytes < MAX_PAYLOAD - 1 -> sysex[sysexBytes++] = value.toByte()
            else -> {
                oversizeDelta++
                droppingOversizeSysex = true
            }
        }
    }

    private fun beginSysex(timestamp: Long, countInterrupted: Boolean = true) {
        if (countInterrupted && hasIncompleteShortMessage()) malformedDelta++
        clearPendingShort()
        runningStatus = 0
        inSysex = true
        droppingOversizeSysex = false
        sysexBytes = 1
        sysex[0] = 0xf0.toByte()
        messageTimestamp = timestamp
    }

    private fun resetSysex() {
        inSysex = false
        droppingOversizeSysex = false
        sysexBytes = 0
    }

    private fun beginStatus(status: Int, timestamp: Long) {
        if (hasIncompleteShortMessage()) malformedDelta++
        clearPendingShort()
        if (status == 0xf7) {
            malformedDelta++
            runningStatus = 0
            return
        }

        pendingStatus = status
        shortMessage[0] = status.toByte()
        messageTimestamp = timestamp
        pendingDataBytes = when {
            status in 0x80..0xef -> if ((status and 0xe0) == 0xc0) 1 else 2
            status == 0xf1 || status == 0xf3 -> 1
            status == 0xf2 -> 2
            else -> 0
        }
        runningStatus = if (status in 0x80..0xef) status else 0
        if (pendingDataBytes == 0) {
            when (status) {
                0xf6 -> {
                    append(shortMessage, 0, 1, timestamp)
                    clearPendingShort()
                }
                0xf4, 0xf5 -> {
                    malformedDelta++
                    clearPendingShort()
                }
                else -> {
                    malformedDelta++
                    clearPendingShort()
                }
            }
        }
    }

    private fun consumeDataByte(value: Int, timestamp: Long) {
        if (pendingStatus == 0) {
            if (runningStatus == 0) {
                malformedDelta++
                return
            }
            pendingStatus = runningStatus
            shortMessage[0] = runningStatus.toByte()
            pendingDataBytes = if ((runningStatus and 0xe0) == 0xc0) 1 else 2
            pendingDataCount = 0
            messageTimestamp = timestamp
        }
        shortMessage[pendingDataCount + 1] = value.toByte()
        pendingDataCount++
        if (pendingDataCount != pendingDataBytes) return

        append(shortMessage, 0, pendingDataBytes + 1, messageTimestamp)
        if (pendingStatus >= 0xf0) runningStatus = 0
        clearPendingShort()
    }

    private fun hasIncompleteShortMessage(): Boolean =
        pendingStatus != 0 && pendingDataCount < pendingDataBytes

    private fun clearPendingShort() {
        pendingStatus = 0
        pendingDataBytes = 0
        pendingDataCount = 0
    }

    private fun append(source: ByteArray, sourceOffset: Int, size: Int, timestamp: Long) {
        if (size !in 1..MAX_PAYLOAD) {
            malformedDelta++
            return
        }
        if (batchCount == MAX_EVENTS || size > MAX_PAYLOAD - batchBytes) flush()
        if (batchCount == MAX_EVENTS || size > MAX_PAYLOAD - batchBytes) {
            oversizeDelta++
            return
        }
        System.arraycopy(source, sourceOffset, batch, batchBytes, size)
        timestamps[batchCount] = timestamp
        offsets[batchCount] = batchBytes
        lengths[batchCount] = size
        batchBytes += size
        batchCount++
    }

    /** Flush complete messages and parser deltas at the end of each receiver callback. */
    fun flush() {
        if (batchCount == 0 && oversizeDelta == 0 && malformedDelta == 0) return
        sink.submit(
            timestamps,
            offsets,
            lengths,
            batch,
            batchCount,
            oversizeDelta,
            malformedDelta,
        )
        batchCount = 0
        batchBytes = 0
        oversizeDelta = 0
        malformedDelta = 0
    }

    /** Discard pending/pre-flush messages while retaining parser error accounting. */
    fun discardAndReport() {
        if (hasIncompleteShortMessage() || inSysex) malformedDelta++
        batchCount = 0
        batchBytes = 0
        if (oversizeDelta != 0 || malformedDelta != 0) {
            sink.submit(
                timestamps,
                offsets,
                lengths,
                batch,
                0,
                oversizeDelta,
                malformedDelta,
            )
        }
        oversizeDelta = 0
        malformedDelta = 0
        resetState()
    }

    fun reset() {
        batchCount = 0
        batchBytes = 0
        oversizeDelta = 0
        malformedDelta = 0
        resetState()
    }

    private fun resetState() {
        runningStatus = 0
        clearPendingShort()
        resetSysex()
    }
}

internal fun matchUsbAssignment(
    assignment: UsbMidiPortIdentity,
    endpoints: List<UsbMidiEndpoint>,
): List<UsbMidiEndpoint> = endpoints.filter { endpoint ->
    endpoint.identity.vendorId == assignment.vendorId &&
        endpoint.identity.productId == assignment.productId &&
        endpoint.identity.portNumber == assignment.portNumber &&
        (assignment.serialNumber.isBlank() ||
            endpoint.identity.serialNumber == assignment.serialNumber)
}

/** Pure, serial lifecycle state machine used by both production and JVM fakes. */
internal class UsbMidiCoordinator(
    private val platform: MidiPlatform,
    private val sink: MidiIngressSink,
    private val featureSupported: Boolean = true,
    private val publishState: (UsbMidiState) -> Unit = {},
) {
    private data class EndpointKey(val deviceToken: Int, val portNumber: Int)
    private data class Resolved(
        val trackId: Long,
        val assignment: UsbMidiAssignment,
        val endpoint: UsbMidiEndpoint,
    )
    private data class ActivePort(
        val key: EndpointKey,
        val trackIds: Set<Long>,
        val device: MidiPlatformDevice,
        val port: MidiPlatformPort,
        val receiver: MidiPlatformReceiver,
        val parser: MidiByteStreamParser,
        val handle: Long,
    )

    private var generation = 0L
    private var discoveryStarted = false
    private var sessionRunning = false
    private var desired = emptyMap<Long, UsbMidiAssignment>()
    private var available = emptyList<UsbMidiEndpoint>()
    private val runtime = LinkedHashMap<Long, Pair<MidiAssignmentStatus, Long>>()
    private val activePorts = LinkedHashMap<EndpointKey, ActivePort>()
    private val openedDevices = LinkedHashMap<Int, MidiPlatformDevice>()

    val state = MutableStateFlow(UsbMidiState())

    fun setAssignments(assignments: Map<Long, UsbMidiAssignment>) {
        val next = assignments.mapValues { (trackId, incoming) ->
            val previous = desired[trackId]
            if (incoming.deviceToken == null && previous?.identity == incoming.identity) {
                incoming.copy(deviceToken = previous.deviceToken)
            } else {
                incoming
            }
        }
        val changed = !sameSessionAssignments(desired, next)
        desired = next
        if (sessionRunning && changed) restartSession() else publish()
    }

    fun setSessionTokens(tokens: Map<Long, Int>) {
        val next = desired.mapValues { (trackId, assignment) ->
            assignment.copy(deviceToken = tokens[trackId] ?: assignment.deviceToken)
        }
        val changed =
            routingSignature(desired, available) != routingSignature(next, available)
        val retryUnsuccessful = next.any { (trackId, assignment) ->
            tokens.containsKey(trackId) &&
                resolveEndpoint(assignment, available) != null &&
                (runtime[trackId]?.first == MidiAssignmentStatus.Failed ||
                    runtime[trackId]?.first == MidiAssignmentStatus.Disconnected)
        }
        desired = next
        if (sessionRunning && (changed || retryUnsuccessful)) restartSession() else publish()
    }

    fun setAvailable(endpoints: List<UsbMidiEndpoint>) {
        val nextDesired = desired.mapValues { (_, assignment) ->
            val token = assignment.deviceToken
            if (token != null && matchUsbAssignment(assignment.identity, endpoints)
                    .none { it.deviceToken == token }) {
                assignment.copy(deviceToken = null, sourceHandle = 0L)
            } else {
                assignment
            }
        }
        val changed = endpointSessionKeys(available) != endpointSessionKeys(endpoints) ||
            !sameSessionAssignments(desired, nextDesired) ||
            routingSignature(desired, available) != routingSignature(nextDesired, endpoints)
        available = endpoints
        desired = nextDesired
        if (sessionRunning && changed) restartSession() else publish()
    }

    private fun endpointSessionKeys(
        endpoints: List<UsbMidiEndpoint>,
    ): Set<Pair<UsbMidiPortIdentity, Int>> =
        endpoints.mapTo(mutableSetOf()) { it.identity to it.deviceToken }

    private fun routingSignature(
        assignments: Map<Long, UsbMidiAssignment>,
        endpoints: List<UsbMidiEndpoint>,
    ): Map<Long, Pair<UsbMidiPortIdentity, Int>?> =
        assignments.mapValues { (_, assignment) ->
            resolveEndpoint(assignment, endpoints)?.let { it.identity to it.deviceToken }
        }

    private fun sameSessionAssignments(
        left: Map<Long, UsbMidiAssignment>,
        right: Map<Long, UsbMidiAssignment>,
    ): Boolean {
        if (left.size != right.size || left.keys != right.keys) return false
        return left.all { (trackId, assignment) ->
            val other = right[trackId] ?: return false
            assignment.identity == other.identity &&
                assignment.deviceToken == other.deviceToken
        }
    }

    fun startDiscovery() {
        if (!featureSupported || discoveryStarted) {
            publish()
            return
        }
        discoveryStarted = true
        platform.startDiscovery(::setAvailable)
    }

    fun start() {
        if (!featureSupported) {
            publish()
            return
        }
        startDiscovery()
        if (sessionRunning) {
            publish()
            return
        }
        sessionRunning = true
        restartSession()
    }

    fun stop() {
        generation++
        sessionRunning = false
        runtime.clear()
        closeAll()
        publish()
    }

    fun shutdown() {
        stop()
        if (discoveryStarted) platform.stopDiscovery()
        discoveryStarted = false
    }

    private fun restartSession() {
        generation++
        runtime.clear()
        closeAll()
        if (!sessionRunning) {
            publish()
            return
        }

        val resolved = resolveAll()
        for (item in resolved) {
            runtime[item.trackId] = MidiAssignmentStatus.Opening to 0L
        }
        publish()
        val byDevice = resolved.groupBy { it.endpoint.deviceToken }
        for ((deviceToken, requests) in byDevice) {
            val capturedGeneration = generation
            platform.open(deviceToken) { opened ->
                platform.postControl {
                    onDeviceOpened(capturedGeneration, deviceToken, requests, opened)
                }
            }
        }
    }

    private fun onDeviceOpened(
        capturedGeneration: Long,
        deviceToken: Int,
        requests: List<Resolved>,
        device: MidiPlatformDevice?,
    ) {
        if (capturedGeneration != generation || !sessionRunning) {
            device?.closeSafely()
            return
        }
        if (device == null || device.token != deviceToken) {
            device?.closeSafely()
            mark(requests.map { it.trackId }, MidiAssignmentStatus.Failed)
            return
        }

        openedDevices[deviceToken] = device
        val byPort = requests.groupBy {
            EndpointKey(it.endpoint.deviceToken, it.endpoint.identity.portNumber)
        }
        for ((key, portRequests) in byPort) {
            openPort(capturedGeneration, key, portRequests, device)
        }
        if (activePorts.values.none { it.device === device }) {
            openedDevices.remove(deviceToken)
            device.closeSafely()
        }
        publish()
    }

    private fun openPort(
        capturedGeneration: Long,
        key: EndpointKey,
        requests: List<Resolved>,
        device: MidiPlatformDevice,
    ) {
        val port = device.openOutputPort(key.portNumber)
        if (port == null) {
            mark(requests.map { it.trackId }, MidiAssignmentStatus.Disconnected)
            return
        }
        val identity = requests.first().endpoint.identity
        val handle = sink.register(identity)
        if (handle == 0L) {
            port.closeSafely()
            mark(requests.map { it.trackId }, MidiAssignmentStatus.Failed)
            return
        }

        val parser = MidiByteStreamParser(object : MidiByteStreamParser.BatchSink {
            override fun submit(
                timestamps: LongArray,
                offsets: IntArray,
                lengths: IntArray,
                payload: ByteArray,
                count: Int,
                oversize: Int,
                malformed: Int,
            ) {
                sink.enqueue(
                    handle,
                    timestamps,
                    offsets,
                    lengths,
                    payload,
                    count,
                    oversize,
                    malformed,
                )
            }
        })
        val receiverFailed = AtomicBoolean(false)
        val receiver = object : MidiPlatformReceiver {
            override fun onSend(
                data: ByteArray,
                offset: Int,
                count: Int,
                timestampNanos: Long,
            ) {
                try {
                    synchronized(parser) {
                        parser.onBytes(data, offset, count, timestampNanos)
                    }
                } catch (_: Throwable) {
                    receiverFailed.set(true)
                    platform.postControl { failPort(capturedGeneration, key) }
                }
            }

            override fun onFlush() {
                try {
                    synchronized(parser) {
                        parser.discardAndReport()
                        sink.flush(handle)
                    }
                } catch (_: Throwable) {
                    receiverFailed.set(true)
                    platform.postControl { failPort(capturedGeneration, key) }
                }
            }
        }

        val connected = try {
            port.connect(receiver)
        } catch (_: Throwable) {
            false
        }
        if (!connected || receiverFailed.get() || capturedGeneration != generation) {
            closePortResources(port, receiver, parser, handle)
            mark(requests.map { it.trackId }, MidiAssignmentStatus.Failed)
            return
        }

        val bound = LinkedHashSet<Long>()
        for (request in requests) {
            if (sink.bind(request.trackId, handle)) {
                bound += request.trackId
                runtime[request.trackId] = MidiAssignmentStatus.Connected to handle
            } else {
                runtime[request.trackId] = MidiAssignmentStatus.Failed to 0L
            }
        }
        if (bound.isEmpty()) {
            closePortResources(port, receiver, parser, handle)
            return
        }
        activePorts[key] = ActivePort(
            key,
            bound,
            device,
            port,
            receiver,
            parser,
            handle,
        )
    }

    private fun failPort(capturedGeneration: Long, key: EndpointKey) {
        if (capturedGeneration != generation) return
        val active = activePorts.remove(key) ?: return
        for (trackId in active.trackIds) {
            runtime[trackId] = MidiAssignmentStatus.Failed to 0L
        }
        closePortResources(active.port, active.receiver, active.parser, active.handle)
        closeDeviceIfUnused(active.device)
        publish()
    }

    private fun closeAll() {
        if (activePorts.isEmpty() && openedDevices.isEmpty()) return
        val devices = LinkedHashSet<MidiPlatformDevice>()
        for (active in activePorts.values) {
            devices += active.device
            closePortResources(
                active.port,
                active.receiver,
                active.parser,
                active.handle,
            )
        }
        devices += openedDevices.values
        activePorts.clear()
        openedDevices.clear()
        for (device in devices) device.closeSafely()
    }

    private fun closePortResources(
        port: MidiPlatformPort,
        receiver: MidiPlatformReceiver,
        parser: MidiByteStreamParser,
        handle: Long,
    ) {
        try {
            port.disconnect(receiver)
        } catch (_: Throwable) {
            // Continue deterministic teardown.
        }
        synchronized(parser) {
            try {
                parser.discardAndReport()
            } catch (_: Throwable) {
                parser.reset()
            }
            try {
                sink.flush(handle)
            } catch (_: Throwable) {
                // Unregister still owns final source teardown.
            }
        }
        port.closeSafely()
        try {
            sink.unregister(handle)
        } catch (_: Throwable) {
            // Device resources must still close.
        }
    }

    private fun closeDeviceIfUnused(device: MidiPlatformDevice) {
        if (activePorts.values.any { it.device === device }) return
        openedDevices.remove(device.token)
        device.closeSafely()
    }

    private fun resolveAll(): List<Resolved> = desired.mapNotNull { (trackId, assignment) ->
        resolveEndpoint(assignment)?.let { endpoint ->
            Resolved(trackId, assignment, endpoint)
        }
    }

    private fun resolveEndpoint(
        assignment: UsbMidiAssignment,
        endpoints: List<UsbMidiEndpoint> = available,
    ): UsbMidiEndpoint? {
        val matches = matchUsbAssignment(assignment.identity, endpoints)
        if (matches.isEmpty()) return null
        if (assignment.identity.serialNumber.isBlank()) {
            assignment.deviceToken?.let { token ->
                matches.firstOrNull { it.deviceToken == token }?.let { return it }
            }
        }
        return matches.singleOrNull()
    }


    private fun mark(trackIds: List<Long>, status: MidiAssignmentStatus) {
        for (trackId in trackIds) runtime[trackId] = status to 0L
        publish()
    }

    private fun publish() {
        val assignments = desired.mapValues { (trackId, assignment) ->
            val matches = matchUsbAssignment(assignment.identity, available)
            val selected = resolveEndpoint(assignment)
            if (selected == null) {
                if (matches.size > 1) {
                    assignment.copy(
                        deviceToken = null,
                        status = MidiAssignmentStatus.Ambiguous,
                        sourceHandle = 0L,
                    )
                } else {
                    assignment.copy(
                        deviceToken = null,
                        status = MidiAssignmentStatus.Disconnected,
                        sourceHandle = 0L,
                    )
                }
            } else {
                val runtimeState = runtime[trackId]
                assignment.copy(
                    deviceToken = selected.deviceToken,
                    status = runtimeState?.first ?: MidiAssignmentStatus.Disconnected,
                    sourceHandle = runtimeState?.second ?: 0L,
                )
            }
        }
        val value = UsbMidiState(
            availability = if (featureSupported) {
                MidiAvailability.Available
            } else {
                MidiAvailability.Unsupported
            },
            endpoints = if (featureSupported) available else emptyList(),
            assignments = assignments,
            sessionGeneration = generation,
        )
        state.value = value
        publishState(value)
    }

    private fun MidiPlatformPort.closeSafely() {
        try {
            close()
        } catch (_: Throwable) {
            // Continue resource teardown.
        }
    }

    private fun MidiPlatformDevice.closeSafely() {
        try {
            close()
        } catch (_: Throwable) {
            // Stale callbacks must never escape cleanup.
        }
    }
}

object UsbMidiInputManager {
    private val stateMutable = MutableStateFlow(UsbMidiState())
    val state: StateFlow<UsbMidiState> = stateMutable.asStateFlow()

    @Volatile
    private var initialized = false
    private var handlerThread: HandlerThread? = null
    private var handler: Handler? = null
    private var coordinator: UsbMidiCoordinator? = null
    private var watchdogPosted = false
    private var watchdogWasRunning: Boolean? = null

    private val watchdog = object : Runnable {
        override fun run() {
            watchdogPosted = false
            val current = stateMutable.value
            if (current.availability != MidiAvailability.Available ||
                current.assignments.values.none { it.status == MidiAssignmentStatus.Connected }
            ) {
                watchdogWasRunning = null
                return
            }
            val native = NativeEngine.getInstance()
            val running = runCatching {
                native.nativeIsEngineRunning() && !native.nativeIsEngineError()
            }.getOrDefault(false)
            if (watchdogWasRunning == true && !running) {
                coordinator?.stop()
            }
            watchdogWasRunning = running
            scheduleWatchdog()
        }
    }

    private fun scheduleWatchdog() {
        if (!watchdogPosted) {
            watchdogPosted = true
            handler?.postDelayed(watchdog, 100L)
        }
    }

    fun initialize(context: Context) {
        if (initialized) return
        synchronized(this) {
            if (initialized) return
            val applicationContext = context.applicationContext
            if (!applicationContext.packageManager.hasSystemFeature(PackageManager.FEATURE_MIDI)) {
                stateMutable.value = UsbMidiState(MidiAvailability.Unsupported)
                initialized = true
                return
            }

            val thread = HandlerThread("UsbMidiDiscovery").also { it.start() }
            val controlHandler = Handler(thread.looper)
            val platform = AndroidMidiPlatform(applicationContext, controlHandler)
            val nextCoordinator = UsbMidiCoordinator(
                platform = platform,
                sink = NativeMidiIngressSink(),
                publishState = {
                    stateMutable.value = it
                    if (it.assignments.values.any { assignment ->
                            assignment.status == MidiAssignmentStatus.Connected
                        }) {
                        if (watchdogWasRunning == null) watchdogWasRunning = true
                        scheduleWatchdog()
                    } else {
                        watchdogWasRunning = null
                    }
                },
            )
            handlerThread = thread
            handler = controlHandler
            coordinator = nextCoordinator
            initialized = true
            controlHandler.post { nextCoordinator.startDiscovery() }
        }
    }

    fun findLiveHandle(endpoint: UsbMidiEndpoint): Long {
        val assignment = stateMutable.value.assignments.values.firstOrNull {
            it.status == MidiAssignmentStatus.Connected &&
                it.sourceHandle != 0L &&
                it.identity == endpoint.identity &&
                it.deviceToken == endpoint.deviceToken
        }
        return assignment?.sourceHandle ?: 0L
    }

    fun updateDesiredAssignments(assignments: Map<Long, UsbMidiAssignment>) {
        val controlHandler = handler
        if (controlHandler == null) {
            if (initialized) {
                stateMutable.value = stateMutable.value.copy(assignments = assignments)
            }
            return
        }
        controlHandler.post { coordinator?.setAssignments(assignments) }
    }

    fun updateSessionTokens(tokens: Map<Long, Int>) {
        handler?.post { coordinator?.setSessionTokens(tokens) }
    }

    suspend fun startSessionAndAwait() {
        dispatchAndAwait { it.start() }
    }

    suspend fun stopSessionAndAwait() {
        dispatchAndAwait { it.stop() }
    }

    fun onEngineStateChanged(running: Boolean) {
        if (!running) handler?.post { coordinator?.stop() }
    }

    private suspend fun dispatchAndAwait(action: (UsbMidiCoordinator) -> Unit) {
        val controlHandler = handler ?: return
        val completion = CompletableDeferred<Unit>()
        if (!controlHandler.post {
                coordinator?.let(action)
                completion.complete(Unit)
            }) {
            completion.complete(Unit)
        }
        completion.await()
    }

    private class NativeMidiIngressSink : MidiIngressSink {
        private val native: NativeEngine
            get() = NativeEngine.getInstance()

        override fun register(identity: UsbMidiPortIdentity): Long =
            native.registerUsbMidiSource(
                identity.vendorId,
                identity.productId,
                identity.serialNumber,
                identity.portNumber,
            )

        override fun unregister(handle: Long) = native.unregisterUsbMidiSource(handle)
        override fun flush(handle: Long) = native.flushUsbMidiSource(handle)
        override fun bind(trackId: Long, handle: Long): Boolean =
            native.bindTrackUsbMidiSource(trackId, handle)

        override fun enqueue(
            handle: Long,
            timestamps: LongArray,
            offsets: IntArray,
            lengths: IntArray,
            payload: ByteArray,
            count: Int,
            oversizeDelta: Int,
            malformedDelta: Int,
        ): Int = native.enqueueUsbMidiBatch(
            handle,
            timestamps,
            offsets,
            lengths,
            payload,
            count,
            oversizeDelta,
            malformedDelta,
        )
    }

    private class AndroidMidiPlatform(
        context: Context,
        private val handler: Handler,
    ) : MidiPlatform {
        private val manager = context.getSystemService(Context.MIDI_SERVICE) as MidiManager
        private val infos = HashMap<Int, MidiDeviceInfo>()
        private var callback: MidiManager.DeviceCallback? = null

        override fun enumerate(): List<UsbMidiEndpoint> {
            val current = enumerateInfos()
            infos.clear()
            for (info in current) infos[info.id] = info
            return current.flatMap(::endpoints)
        }

        override fun startDiscovery(onChanged: (List<UsbMidiEndpoint>) -> Unit) {
            if (callback != null) {
                onChanged(enumerate())
                return
            }
            val nextCallback = object : MidiManager.DeviceCallback() {
                override fun onDeviceAdded(info: MidiDeviceInfo) {
                    infos[info.id] = info
                    onChanged(enumerate())
                }

                override fun onDeviceRemoved(info: MidiDeviceInfo) {
                    infos.remove(info.id)
                    onChanged(enumerate())
                }
            }
            callback = nextCallback
            if (Build.VERSION.SDK_INT >= 33) {
                manager.registerDeviceCallback(
                    MidiManager.TRANSPORT_MIDI_BYTE_STREAM,
                    Executor { command -> handler.post(command) },
                    nextCallback,
                )
            } else {
                @Suppress("DEPRECATION")
                manager.registerDeviceCallback(nextCallback, handler)
            }
            onChanged(enumerate())
        }

        override fun stopDiscovery() {
            callback?.let(manager::unregisterDeviceCallback)
            callback = null
        }

        override fun open(token: Int, callback: (MidiPlatformDevice?) -> Unit) {
            val info = infos[token]
            if (info == null) {
                callback(null)
                return
            }
            manager.openDevice(
                info,
                { device -> callback(device?.let(::AndroidMidiDevice)) },
                handler,
            )
        }

        override fun postControl(block: () -> Unit) {
            handler.post(block)
        }

        @Suppress("DEPRECATION")
        private fun enumerateInfos(): List<MidiDeviceInfo> =
            if (Build.VERSION.SDK_INT >= 33) {
                manager.getDevicesForTransport(MidiManager.TRANSPORT_MIDI_BYTE_STREAM).toList()
            } else {
                manager.devices.toList()
            }

        @Suppress("DEPRECATION")
        private fun endpoints(info: MidiDeviceInfo): List<UsbMidiEndpoint> {
            if (info.type != MidiDeviceInfo.TYPE_USB) return emptyList()
            val usb = info.properties.getParcelable<UsbDevice>(
                MidiDeviceInfo.PROPERTY_USB_DEVICE,
            ) ?: return emptyList()
            val serial = info.properties.getString(
                MidiDeviceInfo.PROPERTY_SERIAL_NUMBER,
            ).orEmpty()
            val rawLabel = listOfNotNull(
                info.properties.getString(MidiDeviceInfo.PROPERTY_MANUFACTURER),
                info.properties.getString(MidiDeviceInfo.PROPERTY_PRODUCT),
                info.properties.getString(MidiDeviceInfo.PROPERTY_NAME),
            ).firstOrNull { it.isNotBlank() }
                ?: "USB MIDI ${usb.vendorId.toString(16)}:${usb.productId.toString(16)}"
            return info.ports.asSequence()
                .filter { it.type == MidiDeviceInfo.PortInfo.TYPE_OUTPUT }
                .map { port ->
                    val identity = UsbMidiPortIdentity(
                        usb.vendorId,
                        usb.productId,
                        serial,
                        port.portNumber,
                    )
                    UsbMidiEndpoint(
                        identity,
                        sanitizeUsbMidiDisplayName("$rawLabel • ${port.name ?: "Port ${port.portNumber}"}"),
                        info.id,
                    )
                }
                .toList()
        }

        private class AndroidMidiDevice(
            private val device: MidiDevice,
        ) : MidiPlatformDevice {
            override val token: Int
                get() = device.info.id

            override fun openOutputPort(portNumber: Int): MidiPlatformPort? =
                device.openOutputPort(portNumber)?.let(::AndroidMidiPort)

            override fun close() = device.close()
        }

        private class AndroidMidiPort(
            private val port: MidiOutputPort,
        ) : MidiPlatformPort {
            private val receivers = IdentityHashMap<MidiPlatformReceiver, MidiReceiver>()

            override val portNumber: Int
                get() = port.portNumber

            override fun connect(receiver: MidiPlatformReceiver): Boolean {
                val androidReceiver = object : MidiReceiver() {
                    override fun onSend(
                        msg: ByteArray,
                        offset: Int,
                        count: Int,
                        timestamp: Long,
                    ) {
                        receiver.onSend(msg, offset, count, timestamp)
                    }

                    override fun onFlush() {
                        receiver.onFlush()
                    }
                }
                return try {
                    port.connect(androidReceiver)
                    receivers[receiver] = androidReceiver
                    true
                } catch (_: Throwable) {
                    false
                }
            }

            override fun disconnect(receiver: MidiPlatformReceiver) {
                receivers.remove(receiver)?.let(port::disconnect)
            }

            override fun close() {
                receivers.clear()
                port.close()
            }
        }
    }
}

internal fun sanitizeUsbMidiDisplayName(value: String): String {
    val builder = StringBuilder()
    var index = 0
    var codePoints = 0
    var utf8Bytes = 0
    while (index < value.length && codePoints < 48) {
        val codePoint = value.codePointAt(index)
        if (!Character.isISOControl(codePoint) && codePoint != 0xfffd) {
            val encodedBytes = when {
                codePoint <= 0x7f -> 1
                codePoint <= 0x7ff -> 2
                codePoint <= 0xffff -> 3
                else -> 4
            }
            if (utf8Bytes + encodedBytes > 288) break
            builder.appendCodePoint(codePoint)
            utf8Bytes += encodedBytes
            codePoints++
        }
        index += Character.charCount(codePoint)
    }
    return builder.toString().trim().ifEmpty { "USB MIDI" }
}
