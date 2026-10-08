package com.example.androidpendisplay

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Reassembly, FEC recovery and loss accounting, driven with synthetic
 * datagrams built exactly the way the PC builds them (Packets.h).
 */
class VideoReassemblerTest {

    private val magic = 0xabba
    private val version = 0x02
    private val headerSize = 19
    private val fragmentPayload = 1280

    private fun frameBytes(size: Int, seed: Int): ByteArray =
        ByteArray(size) { ((it * 31 + seed) and 0xff).toByte() }

    private fun dataPacket(
        frame: ByteArray,
        frameId: Int,
        index: Int,
        sessionId: Int = 0,
        timestamp: Int = 0,
    ): ByteArray {
        val count = (frame.size + fragmentPayload - 1) / fragmentPayload
        val offset = index * fragmentPayload
        val length = minOf(fragmentPayload, frame.size - offset)
        return packet(sessionId, frameId, timestamp, index, count, frame, offset, length)
    }

    private fun parityPacket(
        frame: ByteArray,
        frameId: Int,
        group: Int,
        sessionId: Int = 0,
    ): ByteArray {
        val count = (frame.size + fragmentPayload - 1) / fragmentPayload
        val protectedCount = count - 1
        val begin = group * VideoReassembler.FEC_GROUP_SIZE
        val end = minOf(begin + VideoReassembler.FEC_GROUP_SIZE, protectedCount)
        val parity = ByteArray(fragmentPayload)
        for (index in begin until end) {
            val offset = index * fragmentPayload
            for (byte in 0 until fragmentPayload) {
                parity[byte] = (parity[byte].toInt() xor frame[offset + byte].toInt()).toByte()
            }
        }
        return packet(sessionId, frameId, 0, count + group, count, parity, 0, fragmentPayload)
    }

    private fun packet(
        sessionId: Int,
        frameId: Int,
        timestamp: Int,
        fragmentIndex: Int,
        fragmentCount: Int,
        source: ByteArray,
        sourceOffset: Int,
        length: Int,
    ): ByteArray {
        val buffer = ByteBuffer.allocate(headerSize + length).order(ByteOrder.BIG_ENDIAN)
        buffer.putShort(magic.toShort())
        buffer.put(version.toByte())
        buffer.putInt(sessionId)
        buffer.putShort(frameId.toShort())
        buffer.putInt(timestamp)
        buffer.putShort(fragmentIndex.toShort())
        buffer.putShort(fragmentCount.toShort())
        buffer.putShort(length.toShort())
        buffer.put(source, sourceOffset, length)
        return buffer.array()
    }

    @Test
    fun `reassembles a multi-fragment frame`() {
        val frame = frameBytes(fragmentPayload * 2 + 500, 3)
        var delivered: ByteArray? = null
        val reassembler = VideoReassembler(onFrame = { data, _, _ -> delivered = data })

        for (index in 0 until 3) {
            val packet = dataPacket(frame, frameId = 1, index = index)
            reassembler.accept(packet, packet.size)
        }

        assertArrayEquals(frame, delivered)
    }

    @Test
    fun `single fragment frame is delivered without reassembly`() {
        val frame = frameBytes(400, 9)
        var delivered: ByteArray? = null
        val reassembler = VideoReassembler(onFrame = { data, _, _ -> delivered = data })
        val packet = dataPacket(frame, frameId = 7, index = 0)

        reassembler.accept(packet, packet.size)

        assertArrayEquals(frame, delivered)
    }

    @Test
    fun `FEC rebuilds a lost fragment instead of dropping the frame`() {
        // 9 fragments: 8 protected (one full FEC group) plus a short tail.
        val frame = frameBytes(fragmentPayload * 8 + 200, 5)
        var delivered: ByteArray? = null
        var lostCalled = false
        val reassembler = VideoReassembler(
            onFrame = { data, _, _ -> delivered = data },
            onFrameLost = { lostCalled = true },
        )

        for (index in 0 until 9) {
            if (index == 4) continue  // The fragment that goes missing.
            val packet = dataPacket(frame, frameId = 2, index = index)
            reassembler.accept(packet, packet.size)
        }
        assertNull(delivered)

        val parity = parityPacket(frame, frameId = 2, group = 0)
        reassembler.accept(parity, parity.size)

        assertArrayEquals(frame, delivered)
        assertTrue("no PLI should be needed when FEC covered the loss", !lostCalled)
        assertEquals(1L, reassembler.consumeStats().recoveredFragments)
    }

    /**
     * The normal wire order: every data fragment, then the parity. Nothing
     * was lost, so nothing may be reported lost - no PLI, no loss, exactly
     * one delivery. This is the case that used to resurrect the completed
     * frame as an empty "ghost" and request a keyframe on every frame.
     */
    @Test
    fun `parity arriving after a complete frame is ignored`() {
        val frame = frameBytes(fragmentPayload * 12 + 700, 12)  // 13 fragments, like a real P-frame
        val delivered = mutableListOf<Int>()
        var lostCalls = 0
        val reassembler = VideoReassembler(
            onFrame = { _, _, id -> delivered.add(id) },
            onFrameLost = { lostCalls++ },
        )

        val fragmentCount = (frame.size + fragmentPayload - 1) / fragmentPayload
        for (index in 0 until fragmentCount) {
            val packet = dataPacket(frame, frameId = 40, index = index)
            reassembler.accept(packet, packet.size)
        }
        for (group in 0 until VideoReassembler.fecGroupCount(fragmentCount)) {
            val parity = parityPacket(frame, frameId = 40, group = group)
            reassembler.accept(parity, parity.size)
        }

        // Let any pending entry age past the 80ms timeout, then drive the
        // cleanup with an unrelated single-fragment frame.
        Thread.sleep(120)
        val next = dataPacket(frameBytes(200, 1), frameId = 41, index = 0)
        reassembler.accept(next, next.size)

        assertEquals(listOf(40, 41), delivered)
        assertEquals("no keyframe request when nothing was lost", 0, lostCalls)
        assertEquals(0, reassembler.consumeStats().lossPermille)
    }

    @Test
    fun `late data for a frame already completed by FEC is ignored`() {
        val frame = frameBytes(fragmentPayload * 8 + 200, 13)
        val delivered = mutableListOf<Int>()
        var lostCalls = 0
        val reassembler = VideoReassembler(
            onFrame = { _, _, id -> delivered.add(id) },
            onFrameLost = { lostCalls++ },
        )

        // Fragment 4 is late, not lost: FEC completes the frame first.
        for (index in 0 until 9) {
            if (index == 4) continue
            val packet = dataPacket(frame, frameId = 50, index = index)
            reassembler.accept(packet, packet.size)
        }
        val parity = parityPacket(frame, frameId = 50, group = 0)
        reassembler.accept(parity, parity.size)
        val late = dataPacket(frame, frameId = 50, index = 4)
        reassembler.accept(late, late.size)

        Thread.sleep(120)
        val next = dataPacket(frameBytes(200, 1), frameId = 51, index = 0)
        reassembler.accept(next, next.size)

        assertEquals(listOf(50, 51), delivered)
        assertEquals(0, lostCalls)
    }

    @Test
    fun `two losses in one group cannot be recovered`() {
        val frame = frameBytes(fragmentPayload * 8 + 200, 6)
        var delivered: ByteArray? = null
        val reassembler = VideoReassembler(onFrame = { data, _, _ -> delivered = data })

        for (index in 0 until 9) {
            if (index == 2 || index == 5) continue
            val packet = dataPacket(frame, frameId = 3, index = index)
            reassembler.accept(packet, packet.size)
        }
        val parity = parityPacket(frame, frameId = 3, group = 0)
        reassembler.accept(parity, parity.size)

        assertNull(delivered)
    }

    @Test
    fun `packets from another session are ignored`() {
        val frame = frameBytes(300, 4)
        var delivered: ByteArray? = null
        val reassembler = VideoReassembler(
            onFrame = { data, _, _ -> delivered = data },
            expectedSessionId = { 0x1234 },
        )

        val foreign = dataPacket(frame, frameId = 1, index = 0, sessionId = 0x9999)
        reassembler.accept(foreign, foreign.size)
        assertNull(delivered)

        val ours = dataPacket(frame, frameId = 2, index = 0, sessionId = 0x1234)
        reassembler.accept(ours, ours.size)
        assertArrayEquals(frame, delivered)
    }

    @Test
    fun `malformed packets are rejected`() {
        val frame = frameBytes(300, 8)
        var delivered: ByteArray? = null
        val reassembler = VideoReassembler(onFrame = { data, _, _ -> delivered = data })

        val wrongMagic = dataPacket(frame, 1, 0).also { it[0] = 0 }
        reassembler.accept(wrongMagic, wrongMagic.size)

        val wrongVersion = dataPacket(frame, 2, 0).also { it[2] = 1 }
        reassembler.accept(wrongVersion, wrongVersion.size)

        // PayloadLength that disagrees with the datagram length.
        val lying = dataPacket(frame, 3, 0)
        ByteBuffer.wrap(lying).order(ByteOrder.BIG_ENDIAN).putShort(17, 999)
        reassembler.accept(lying, lying.size)

        assertNull(delivered)
    }

    @Test
    fun `loss is reported per fragment`() {
        // Two fragments expected, one arrives: 500 per mille.
        val frame = frameBytes(fragmentPayload + 100, 2)
        val reassembler = VideoReassembler(onFrame = { _, _, _ -> })
        val packet = dataPacket(frame, frameId = 1, index = 0)
        reassembler.accept(packet, packet.size)

        assertEquals(500, reassembler.consumeStats().lossPermille)
        // Draining resets the window.
        assertEquals(0, reassembler.consumeStats().lossPermille)
    }

    @Test
    fun `frames older than the last completed one are dropped`() {
        val frame = frameBytes(300, 1)
        val delivered = mutableListOf<Int>()
        val reassembler = VideoReassembler(onFrame = { _, _, id -> delivered.add(id) })

        // FrameID is 2 bytes and wraps, so "newer" is the RFC 1982 circular
        // comparison, not a plain >. 65535 -> 0 is forward; 1 -> 65535 is not.
        for (id in listOf(65534, 65535, 0, 1)) {
            val packet = dataPacket(frame, frameId = id, index = 0)
            reassembler.accept(packet, packet.size)
        }
        val stale = dataPacket(frame, frameId = 65535, index = 0)
        reassembler.accept(stale, stale.size)

        assertEquals(listOf(65534, 65535, 0, 1), delivered)
    }
}
