package com.vibes.dsp.engine

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Black-box tests for the byte-stream boundary.  RecordingSink copies each
 * submission because the production parser deliberately reuses its arrays.
 */
class UsbMidiInputManagerTest {
    @Test
    fun arbitraryCallbackSplitsPreserveCompleteMessagesAndOrder() {
        val sink = RecordingSink()
        val parser = MidiByteStreamParser(sink)

        parser.onBytes(byteArrayOf(0x90.toByte()), 0, 1, 100)
        parser.onBytes(byteArrayOf(0x3c), 0, 1, 200)
        parser.onBytes(byteArrayOf(0x64, 0x90.toByte(), 0x3d, 0x65), 0, 4, 300)

        assertEquals(
            listOf(
                Event(100, byteArrayOf(0x90.toByte(), 0x3c, 0x64)),
                Event(300, byteArrayOf(0x90.toByte(), 0x3d, 0x65)),
            ),
            sink.events,
        )
    }

    @Test
    fun explicitStatusUsesStatusTimestampButRunningStatusUsesFirstDataTimestamp() {
        val sink = RecordingSink()
        val parser = MidiByteStreamParser(sink)

        parser.onBytes(byteArrayOf(0x90.toByte(), 0x40), 0, 2, 10)
        parser.onBytes(byteArrayOf(0x7f), 0, 1, 20)
        parser.onBytes(byteArrayOf(0x41, 0x7e), 0, 2, 30)

        assertEquals(listOf(10L, 30L), sink.events.map { it.timestamp })
        assertPayloads(
            listOf(
                byteArrayOf(0x90.toByte(), 0x40, 0x7f),
                byteArrayOf(0x90.toByte(), 0x41, 0x7e),
            ),
            sink.events.map { it.bytes },
        )
    }

    @Test
    fun channelAndSystemCommonLengthsAreDecodedWithoutBorrowingRunningStatus() {
        val sink = RecordingSink()
        val parser = MidiByteStreamParser(sink)

        parser.onBytes(
            byteArrayOf(
                0xc0.toByte(), 0x07,
                0xd0.toByte(), 0x11,
                0xf1.toByte(), 0x01,
                0xf2.toByte(), 0x02, 0x03,
                0xf3.toByte(), 0x04,
                0xf6.toByte(),
            ),
            0,
            12,
            50,
        )

        assertPayloads(
            listOf(
                byteArrayOf(0xc0.toByte(), 0x07),
                byteArrayOf(0xd0.toByte(), 0x11),
                byteArrayOf(0xf1.toByte(), 0x01),
                byteArrayOf(0xf2.toByte(), 0x02, 0x03),
                byteArrayOf(0xf3.toByte(), 0x04),
                byteArrayOf(0xf6.toByte()),
            ),
            sink.events.map { it.bytes },
        )
    }

    @Test
    fun realtimeMessagesEmitImmediatelyAndDoNotDisturbRunningStatus() {
        val sink = RecordingSink()
        val parser = MidiByteStreamParser(sink)

        parser.onBytes(
            byteArrayOf(0x90.toByte(), 0x40, 0xf8.toByte(), 0x7f, 0xfa.toByte(), 0x41, 0x7e),
            0,
            7,
            70,
        )

        assertPayloads(
            listOf(
                byteArrayOf(0xf8.toByte()),
                byteArrayOf(0x90.toByte(), 0x40, 0x7f),
                byteArrayOf(0xfa.toByte()),
                byteArrayOf(0x90.toByte(), 0x41, 0x7e),
            ),
            sink.events.map { it.bytes },
        )
    }

    @Test
    fun systemCommonClearsRunningStatusBeforeFollowingData() {
        val sink = RecordingSink()
        val parser = MidiByteStreamParser(sink)

        parser.onBytes(
            byteArrayOf(
                0x90.toByte(), 0x40, 0x7f,
                0xf1.toByte(), 0x01,
                0x02,
            ),
            0,
            6,
            81,
        )

        assertEquals(1, sink.malformed)
        assertPayloads(
            listOf(
                byteArrayOf(0x90.toByte(), 0x40, 0x7f),
                byteArrayOf(0xf1.toByte(), 0x01),
            ),
            sink.events.map { it.bytes },
        )
    }

    @Test
    fun fragmentedSysExUsesF0TimestampAndPreservesBytesExactly() {
        val sink = RecordingSink()
        val parser = MidiByteStreamParser(sink)

        parser.onBytes(byteArrayOf(0xf0.toByte(), 0x01, 0x02), 0, 3, 101)
        parser.onBytes(byteArrayOf(0x03, 0x04), 0, 2, 202)
        parser.onBytes(byteArrayOf(0xf7.toByte()), 0, 1, 303)

        assertEquals(listOf(Event(101, byteArrayOf(0xf0.toByte(), 1, 2, 3, 4, 0xf7.toByte()))), sink.events)
    }

    @Test
    fun exactly64KiBSysExIsAcceptedAsOneMessage() {
        val sink = RecordingSink()
        val parser = MidiByteStreamParser(sink)
        val message = ByteArray(65_536)
        message[0] = 0xf0.toByte()
        for (i in 1 until message.lastIndex) message[i] = (i and 0x7f).toByte()
        message[message.lastIndex] = 0xf7.toByte()

        parser.onBytes(message, 0, message.size, 404)

        assertEquals(1, sink.events.size)
        assertEquals(message.toList(), sink.events.single().bytes.toList())
        assertEquals(0, sink.oversize)
    }

    @Test
    fun oversizedSysExDrainsThroughF7AndDoesNotLeakSuffix() {
        val sink = RecordingSink()
        val parser = MidiByteStreamParser(sink)
        val body = ByteArray(65_535) { 0x01 }
        val input = ByteArray(1 + body.size + 1 + 3 + 3)
        input[0] = 0xf0.toByte()
        body.copyInto(input, 1)
        input[1 + body.size] = 0xf7.toByte()
        input[1 + body.size + 1] = 0x90.toByte()
        input[1 + body.size + 2] = 0x40
        input[1 + body.size + 3] = 0x7f
        input[1 + body.size + 4] = 0x01
        input[1 + body.size + 5] = 0x02

        parser.onBytes(input, 0, input.size, 505)

        assertEquals(1, sink.oversize)
        assertPayloads(
            listOf(
                byteArrayOf(0x90.toByte(), 0x40, 0x7f),
                byteArrayOf(0x90.toByte(), 0x01, 0x02),
            ),
            sink.events.map { it.bytes },
        )
    }

    @Test
    fun malformedNestedAndStrayBytesRecoverAtNextValidStatus() {
        val sink = RecordingSink()
        val parser = MidiByteStreamParser(sink)

        parser.onBytes(
            byteArrayOf(
                0x01,                         // stray data
                0xf7.toByte(),                // stray terminator
                0xf0.toByte(), 0x01, 0xf0.toByte(), 0x02, 0xf7.toByte(),
                0xf0.toByte(), 0x03, 0x90.toByte(), 0x40, 0x7f,
            ),
            0,
            12,
            606,
        )

        assertEquals(4, sink.malformed)
        assertPayloads(
            listOf(
                byteArrayOf(0xf0.toByte(), 0x02, 0xf7.toByte()),
                byteArrayOf(0x90.toByte(), 0x40, 0x7f),
            ),
            sink.events.map { it.bytes },
        )
    }

    @Test
    fun zeroAndFutureCallbackTimestampsNormalizeToCallbackClock() {
        val sink = RecordingSink()
        val parser = MidiByteStreamParser(sink) { 1_000L }

        parser.onBytes(byteArrayOf(0xf8.toByte()), 0, 1, 0)
        parser.onBytes(byteArrayOf(0xfa.toByte()), 0, 1, Long.MAX_VALUE)

        assertEquals(listOf(1_000L, 1_000L), sink.events.map { it.timestamp })
        assertFalse(sink.events.any { it.timestamp == Long.MAX_VALUE })
    }

    @Test
    fun batchEventCountBoundaryFlushesCompleteMessagesWithoutDroppingOrder() {
        val sink = RecordingSink()
        val parser = MidiByteStreamParser(sink)

        val realtime = ByteArray(129) { 0xf8.toByte() }
        parser.onBytes(realtime, 0, realtime.size, 707)

        assertEquals(listOf(128, 1), sink.submissions.map { it.count })
        assertEquals(129, sink.events.size)
        assertTrue(sink.events.all { it.bytes.contentEquals(byteArrayOf(0xf8.toByte())) })
    }

    @Test
    fun callbackOffsetAndLengthExcludeBytesOutsideReportedWindow() {
        val sink = RecordingSink()
        val parser = MidiByteStreamParser(sink)
        val callback = byteArrayOf(0x01, 0x90.toByte(), 0x40, 0x7f, 0x02)

        parser.onBytes(callback, 1, 3, 808)

        assertEquals(listOf(Event(808, byteArrayOf(0x90.toByte(), 0x40, 0x7f))), sink.events)
        assertEquals(0, sink.malformed)
    }

    @Test
    fun payloadBoundaryFlushesBeforeNextCompleteMessage() {
        val sink = RecordingSink()
        val parser = MidiByteStreamParser(sink)
        val sysex = ByteArray(65_536)
        sysex[0] = 0xf0.toByte()
        sysex[sysex.lastIndex] = 0xf7.toByte()

        parser.onBytes(sysex + byteArrayOf(0xf8.toByte()), 0, sysex.size + 1, 808)

        assertEquals(listOf(1, 1), sink.submissions.map { it.count })
        assertEquals(0, sink.oversize)
        assertPayloads(
            listOf(sysex, byteArrayOf(0xf8.toByte())),
            sink.events.map { it.bytes },
        )
    }

    @Test
    fun resetDropsHeldMessageAndAllowsFreshRunningStatus() {
        val sink = RecordingSink()
        val parser = MidiByteStreamParser(sink)

        parser.onBytes(byteArrayOf(0x90.toByte(), 0x40), 0, 2, 909)
        parser.reset()
        parser.onBytes(byteArrayOf(0x7f), 0, 1, 910)
        parser.onBytes(byteArrayOf(0x91.toByte(), 0x41, 0x7e), 0, 3, 911)

        assertEquals(1, sink.malformed)
        assertEquals(listOf(Event(911, byteArrayOf(0x91.toByte(), 0x41, 0x7e))), sink.events)
    }

    private data class Event(val timestamp: Long, val bytes: ByteArray) {
        override fun equals(other: Any?): Boolean =
            other is Event && timestamp == other.timestamp && bytes.contentEquals(other.bytes)

        override fun hashCode(): Int = 31 * timestamp.hashCode() + bytes.contentHashCode()
    }

    private data class Submission(val count: Int, val oversize: Int, val malformed: Int)

    @Test
    fun unsupportedPlatformPublishesEmptyStateWithoutTouchingPlatformOrSink() {
        val platform = FakePlatform()
        val sink = FakeSink()
        val coordinator = UsbMidiCoordinator(platform, sink, featureSupported = false)

        coordinator.setAssignments(mapOf(7L to assignment(serial = "device-7")))
        coordinator.start()

        assertEquals(MidiAvailability.Unsupported, coordinator.state.value.availability)
        assertTrue(coordinator.state.value.endpoints.isEmpty())
        assertTrue(platform.enumerateCalls == 0)
        assertTrue(sink.calls.isEmpty())
    }

    @Test
    fun exactSerialMatchingDoesNotCrossConnectAnotherDevice() {
        val assignment = UsbMidiPortIdentity(0x1234, 0x5678, "A", 2)
        val endpoints = listOf(
            endpoint(assignment, 11),
            endpoint(assignment.copy(serialNumber = "B"), 12),
            endpoint(assignment.copy(portNumber = 3), 13),
        )

        assertEquals(listOf(11), matchUsbAssignment(assignment, endpoints).map { it.deviceToken })
    }

    @Test
    fun seriallessAssignmentRestoresOnlyAgainstOneMatchingEndpoint() {
        val identity = UsbMidiPortIdentity(10, 20, "", 1)
        val matches = matchUsbAssignment(identity, listOf(endpoint(identity, 42)))

        assertEquals(1, matches.size)
        assertEquals(42, matches.single().deviceToken)
    }
    @Test
    fun seriallessAssignmentRemainsAmbiguousForTwoMatchingEndpoints() {
        val identity = UsbMidiPortIdentity(10, 20, "", 1)
        val matches = matchUsbAssignment(
            identity,
            listOf(endpoint(identity, 42), endpoint(identity, 43)),
        )

        assertEquals(listOf(42, 43), matches.map { it.deviceToken })
        val coordinator = UsbMidiCoordinator(FakePlatform(), FakeSink())
        coordinator.setAssignments(mapOf(9L to assignment(serial = "")))
        coordinator.setAvailable(listOf(endpoint(identity, 42), endpoint(identity, 43)))

        assertEquals(
            MidiAssignmentStatus.Ambiguous,
            coordinator.state.value.assignments.getValue(9L).status,
        )
    }

    @Test
    fun detachClearsTransientSelectionTokenButRetainsDesiredIdentity() {
        val identity = UsbMidiPortIdentity(10, 20, "serial", 1)
        val coordinator = UsbMidiCoordinator(FakePlatform(), FakeSink())
        coordinator.setAssignments(
            mapOf(
                9L to UsbMidiAssignment(
                    identity = identity,
                    displayName = "Saved",
                    deviceToken = 42,
                ),
            ),
        )
        coordinator.setAvailable(listOf(endpoint(identity, 42)))
        coordinator.setAvailable(emptyList())

        val detached = coordinator.state.value.assignments.getValue(9L)
        assertEquals(identity, detached.identity)
        assertEquals("Saved", detached.displayName)
        assertNull(detached.deviceToken)
        assertEquals(MidiAssignmentStatus.Disconnected, detached.status)
    }

    @Test
    fun reconnectReassociatesPersistedIdentityWithNewSessionToken() {
        val identity = UsbMidiPortIdentity(10, 20, "serial", 1)
        val coordinator = UsbMidiCoordinator(FakePlatform(), FakeSink())
        coordinator.setAssignments(mapOf(9L to UsbMidiAssignment(identity, "Saved")))

        coordinator.setAvailable(listOf(endpoint(identity, 41)))
        assertEquals(41, coordinator.state.value.assignments.getValue(9L).deviceToken)
        coordinator.setAvailable(emptyList())
        assertNull(coordinator.state.value.assignments.getValue(9L).deviceToken)
        coordinator.setAvailable(listOf(endpoint(identity, 99)))

        val reconnected = coordinator.state.value.assignments.getValue(9L)
        assertEquals(identity, reconnected.identity)
        assertEquals(99, reconnected.deviceToken)
        assertEquals(MidiAssignmentStatus.Disconnected, reconnected.status)
    }

    @Test
    fun coordinatorSessionGenerationIncreasesOnEveryStartAndStop() {
        val coordinator = UsbMidiCoordinator(FakePlatform(), FakeSink())

        coordinator.start()
        val afterStart = coordinator.state.value.sessionGeneration
        coordinator.stop()
        val afterStop = coordinator.state.value.sessionGeneration
        coordinator.start()
        val afterRestart = coordinator.state.value.sessionGeneration

        assertTrue(afterStart > 0)
        assertTrue(afterStop > afterStart)
        assertTrue(afterRestart > afterStop)
    }
    @Test
    fun startPublishesPlatformEndpointsAndMatchesDesiredAssignments() {
        val identity = UsbMidiPortIdentity(10, 20, "serial", 1)
        val platform = FakePlatform().apply {
            endpoints = listOf(endpoint(identity, 77))
        }
        val coordinator = UsbMidiCoordinator(platform, FakeSink())
        coordinator.setAssignments(mapOf(9L to UsbMidiAssignment(identity, "Saved")))

        coordinator.start()

        assertEquals(1, platform.enumerateCalls)
        assertEquals(listOf(endpoint(identity, 77)), coordinator.state.value.endpoints)
        assertEquals(77, coordinator.state.value.assignments.getValue(9L).deviceToken)
    }

    @Test
    fun groupedPortsOpenOneDeviceAndStopTeardownIsDisconnectCloseUnregisterThenDeviceClose() {
        val first = UsbMidiPortIdentity(10, 20, "serial", 0)
        val second = first.copy(portNumber = 1)
        val firstPort = FakePort(0)
        val secondPort = FakePort(1)
        val device = FakeDevice(7, listOf(firstPort, secondPort))
        val platform = LifecyclePlatform(
            listOf(endpoint(first, 7), endpoint(second, 7)),
            device,
        )
        firstPort.platformEvents = platform.events
        secondPort.platformEvents = platform.events
        val sink = FakeSink(platform.events)
        val coordinator = UsbMidiCoordinator(platform, sink)
        coordinator.setAssignments(
            mapOf(
                1L to UsbMidiAssignment(first, "one"),
                2L to UsbMidiAssignment(second, "two"),
            ),
        )
        coordinator.setAvailable(platform.endpoints)
        assertTrue(platform.openTokens.isEmpty())
        coordinator.start()
        coordinator.stop()

        assertEquals(1, platform.openTokens.count { it == 7 })
        assertEquals(
            listOf(
                "start-discovery", "open-7",
                "register-1", "connect-0", "bind-1",
                "register-2", "connect-1", "bind-2",
                "disconnect-0", "flush-1", "close-port-0", "unregister-1",
                "disconnect-1", "flush-2", "close-port-1", "unregister-2",
                "close-device-7",
            ),
            platform.events,
        )
    }

    @Test
    fun staleOpenCallbackClosesReturnedDeviceWithoutRegisteringOrConnecting() {
        val identity = UsbMidiPortIdentity(10, 20, "serial", 0)
        val platform = LifecyclePlatform(listOf(endpoint(identity, 7)), null)
        val returned = FakeDevice(7, listOf(FakePort(0)))
        platform.deferredDevice = returned
        val sink = FakeSink(platform.events)
        val coordinator = UsbMidiCoordinator(platform, sink)

        coordinator.setAssignments(mapOf(1L to UsbMidiAssignment(identity, "one")))
        coordinator.setAvailable(platform.endpoints)
        coordinator.start()
        coordinator.stop()
        platform.completeOpen()

        assertEquals(listOf("start-discovery", "open-7", "close-device-7"), platform.events)
        assertTrue(sink.calls.none { it.startsWith("register") })
    }

    @Test
    fun receiverConnectionFailureFlushesParserThenClosesPortBeforeUnregistering() {
        val identity = UsbMidiPortIdentity(10, 20, "serial", 0)
        val port = FakePort(
            0,
            connectResult = false,
            preFailureBytes = byteArrayOf(0x90.toByte(), 0x40),
        )
        val device = FakeDevice(7, listOf(port))
        val platform = LifecyclePlatform(listOf(endpoint(identity, 7)), device)
        port.platformEvents = platform.events
        val sink = FakeSink(platform.events)
        val coordinator = UsbMidiCoordinator(platform, sink)

        coordinator.setAssignments(mapOf(1L to UsbMidiAssignment(identity, "one")))
        coordinator.setAvailable(platform.endpoints)
        coordinator.start()

        assertEquals(
            listOf(
                "start-discovery", "open-7", "register-1", "connect-0",
                "disconnect-0", "enqueue", "flush-1", "close-port-0",
                "unregister-1", "close-device-7",
            ),
            platform.events,
        )
        assertEquals(listOf("register-1", "enqueue", "flush-1", "unregister-1"), sink.calls)
        assertEquals(MidiAssignmentStatus.Failed, coordinator.state.value.assignments.getValue(1L).status)
    }

    @Test
    fun connectedReceiverFailureTearsDownPortAndDeviceInOrder() {
        val identity = UsbMidiPortIdentity(10, 20, "serial", 0)
        val port = FakePort(0)
        val device = FakeDevice(7, listOf(port))
        val platform = LifecyclePlatform(listOf(endpoint(identity, 7)), device)
        val sink = FakeSink(platform.events, throwOnEnqueue = true)
        val coordinator = UsbMidiCoordinator(platform, sink)

        coordinator.setAssignments(mapOf(1L to UsbMidiAssignment(identity, "one")))
        coordinator.setAvailable(platform.endpoints)
        coordinator.start()
        port.send(byteArrayOf(0xf8.toByte()), 0, 1, 123)

        assertEquals(
            listOf(
                "start-discovery", "open-7", "register-1", "connect-0", "bind-1",
                "enqueue", "disconnect-0", "flush-1", "close-port-0",
                "unregister-1", "close-device-7",
            ),
            platform.events,
        )
        assertEquals(
            listOf("register-1", "bind-1", "enqueue", "flush-1", "unregister-1"),
            sink.calls,
        )
        assertEquals(
            MidiAssignmentStatus.Failed,
            coordinator.state.value.assignments.getValue(1L).status,
        )
    }

    @Test
    fun connectedReceiverForwardsTimestampedBatchToRegisteredSource() {
        val identity = UsbMidiPortIdentity(10, 20, "serial", 0)
        val port = FakePort(0)
        val device = FakeDevice(7, listOf(port))
        val platform = LifecyclePlatform(listOf(endpoint(identity, 7)), device)
        val sink = FakeSink(platform.events)
        val coordinator = UsbMidiCoordinator(platform, sink)

        coordinator.setAssignments(mapOf(1L to UsbMidiAssignment(identity, "one")))
        coordinator.setAvailable(platform.endpoints)
        coordinator.start()
        port.send(byteArrayOf(0xf8.toByte(), 0xfa.toByte()), 0, 2, 123)

        assertEquals(
            listOf(
                Event(123, byteArrayOf(0xf8.toByte())),
                Event(123, byteArrayOf(0xfa.toByte())),
            ),
            sink.enqueuedEvents,
        )
    }

    private fun assignment(serial: String): UsbMidiAssignment =
        UsbMidiAssignment(
            identity = UsbMidiPortIdentity(10, 20, serial, 1),
            displayName = "Saved",
        )

    private fun endpoint(identity: UsbMidiPortIdentity, token: Int): UsbMidiEndpoint =
        UsbMidiEndpoint(identity = identity, displayName = "Live", deviceToken = token)

    private fun assertPayloads(expected: List<ByteArray>, actual: List<ByteArray>) {
        assertEquals(expected.size, actual.size)
        expected.zip(actual).forEach { (want, got) ->
            assertTrue("MIDI payload differs", want.contentEquals(got))
        }
    }

    private class FakePlatform : MidiPlatform {
        var enumerateCalls = 0
        var endpoints: List<UsbMidiEndpoint> = emptyList()

        override fun enumerate(): List<UsbMidiEndpoint> {
            enumerateCalls++
            return endpoints
        }
    }

    private class FakeSink(
        private val eventLog: MutableList<String>? = null,
        private val throwOnEnqueue: Boolean = false,
    ) : MidiIngressSink {
        val calls = mutableListOf<String>()
        val enqueuedEvents = mutableListOf<Event>()
        private var nextHandle = 1L

        override fun register(identity: UsbMidiPortIdentity): Long {
            val handle = nextHandle++
            calls += "register-$handle"
            eventLog?.add("register-$handle")
            return handle
        }

        override fun unregister(handle: Long) {
            calls += "unregister-$handle"
            eventLog?.add("unregister-$handle")
        }

        override fun flush(handle: Long) {
            calls += "flush-$handle"
            eventLog?.add("flush-$handle")
        }

        override fun bind(trackId: Long, handle: Long): Boolean {
            calls += "bind-$trackId"
            eventLog?.add("bind-$trackId")
            return true
        }

        override fun enqueue(
            handle: Long,
            timestamps: LongArray,
            offsets: IntArray,
            lengths: IntArray,
            payload: ByteArray,
            count: Int,
            oversizeDelta: Int,
            malformedDelta: Int,
        ): Int {
            calls += "enqueue"
            eventLog?.add("enqueue")
            if (throwOnEnqueue) {
                throw IllegalStateException("enqueue failed")
            }
            repeat(count) { index ->
                enqueuedEvents += Event(
                    timestamps[index],
                    payload.copyOfRange(offsets[index], offsets[index] + lengths[index]),
                )
            }
            return count
        }
    }

    private class LifecyclePlatform(
        val endpoints: List<UsbMidiEndpoint>,
        val device: FakeDevice?,
    ) : MidiPlatform {
        val events = mutableListOf<String>()
        val openTokens = mutableListOf<Int>()
        var deferredDevice: FakeDevice? = null
        private var pendingOpen: ((MidiPlatformDevice?) -> Unit)? = null

        init {
            device?.attachEvents(events)
        }

        override fun enumerate(): List<UsbMidiEndpoint> = endpoints

        override fun startDiscovery(onChanged: (List<UsbMidiEndpoint>) -> Unit) {
            events += "start-discovery"
            onChanged(endpoints)
        }

        override fun stopDiscovery() {
            events += "stop-discovery"
        }

        override fun open(token: Int, callback: (MidiPlatformDevice?) -> Unit) {
            events += "open-$token"
            openTokens += token
            if (deferredDevice != null) {
                pendingOpen = callback
            } else {
                device?.attachEvents(events)
                callback(device)
            }
        }

        fun completeOpen() {
            val callback = pendingOpen ?: error("no deferred open")
            pendingOpen = null
            deferredDevice?.attachEvents(events)
            callback(deferredDevice)
        }
    }

    private class FakeDevice(
        override val token: Int,
        val ports: List<FakePort>,
    ) : MidiPlatformDevice {
        private var events: MutableList<String>? = null

        fun attachEvents(log: MutableList<String>) {
            events = log
            ports.forEach { it.platformEvents = log }
        }

        override fun openOutputPort(portNumber: Int): MidiPlatformPort? =
            ports.firstOrNull { it.portNumber == portNumber }

        override fun close() {
            events?.add("close-device-$token")
        }
    }
    private class FakePort(
        override val portNumber: Int,
        var platformEvents: MutableList<String>? = null,
        private val connectResult: Boolean = true,
        private val preFailureBytes: ByteArray? = null,
    ) : MidiPlatformPort {
        private var receiver: MidiPlatformReceiver? = null

        override fun connect(receiver: MidiPlatformReceiver): Boolean {
            platformEvents?.add("connect-$portNumber")
            if (connectResult) {
                this.receiver = receiver
            } else {
                preFailureBytes?.let { receiver.onSend(it, 0, it.size, 123) }
            }
            return connectResult
        }

        override fun disconnect(receiver: MidiPlatformReceiver) {
            platformEvents?.add("disconnect-$portNumber")
            if (this.receiver === receiver) this.receiver = null
        }

        override fun close() {
            platformEvents?.add("close-port-$portNumber")
        }

        fun send(data: ByteArray, offset: Int, count: Int, timestampNanos: Long) {
            checkNotNull(receiver).onSend(data, offset, count, timestampNanos)
        }
    }

    private class RecordingSink : MidiByteStreamParser.BatchSink {
        val events = mutableListOf<Event>()
        val submissions = mutableListOf<Submission>()
        var oversize = 0
        var malformed = 0

        override fun submit(
            timestamps: LongArray,
            offsets: IntArray,
            lengths: IntArray,
            payload: ByteArray,
            count: Int,
            oversize: Int,
            malformed: Int,
        ) {
            submissions += Submission(count, oversize, malformed)
            this.oversize += oversize
            this.malformed += malformed
            repeat(count) { i ->
                events += Event(
                    timestamps[i],
                    payload.copyOfRange(offsets[i], offsets[i] + lengths[i]),
                )
            }
        }
    }
}
