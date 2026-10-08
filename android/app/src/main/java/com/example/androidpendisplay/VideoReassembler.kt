package com.example.androidpendisplay

import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.ConcurrentHashMap

/**
 * Turns UDP datagrams back into H.264 access units.
 *
 * Two things beyond plain reassembly happen here:
 *
 *  - **XOR FEC.** The PC sends one parity fragment per group of 8, so a
 *    single lost fragment inside a group is rebuilt locally instead of
 *    costing the whole frame and a round trip. Parity fragments are
 *    addressed as `fragmentIndex >= fragmentCount`; the frame's last
 *    (short) fragment is deliberately not protected, which is what lets a
 *    recovered fragment be assumed full-length.
 *  - **Loss accounting.** Everything the PC's QoS decides is based on the
 *    numbers collected here, so they are counted per fragment rather than
 *    guessed from frame drops.
 *
 * When a frame has to be abandoned anyway, [onFrameLost] fires so the
 * control channel can ask for a fresh keyframe - without that, a lost
 * fragment corrupts every following P-frame until the next periodic IDR.
 */
class VideoReassembler(
    private val onFrame: (ByteArray, Long, Int) -> Unit,
    private val onFrameLost: () -> Unit = {},
    private val expectedSessionId: () -> Int = { 0 },
) {
    private class Pending(
        val timestampMs: Long,
        val fragmentCount: Int,
        val firstSeenMs: Long,
    ) {
        val fragments = arrayOfNulls<ByteArray>(fragmentCount)
        val parity =
            arrayOfNulls<ByteArray>(VideoReassembler.fecGroupCount(fragmentCount).coerceAtLeast(1))
        var received = 0
    }

    private val pending = ConcurrentHashMap<Int, Pending>()
    private var lastFrameId = -1
    private var datagramCount = 0L
    @Volatile private var completedFrameCount = 0L
    private var discardedFrameCount = 0L
    @Volatile private var lastFrame = -1

    // Per-window counters, drained by consumeStats() once a second. Written
    // on the receive thread, drained from the control channel thread, so
    // volatile: a torn 64-bit read would produce a nonsense loss figure and
    // QoS acts on that figure.
    @Volatile private var windowExpectedFragments = 0L
    @Volatile private var windowReceivedFragments = 0L
    @Volatile private var windowRecoveredFragments = 0L
    @Volatile private var windowCompletedFrames = 0
    @Volatile private var windowDroppedFrames = 0
    @Volatile private var totalRecovered = 0L

    private val tag = "VideoReassembler"

    data class Stats(
        val lossPermille: Int,
        val completedFps: Int,
        val droppedFrames: Int,
        val recoveredFragments: Long,
    )

    fun accept(data: ByteArray, length: Int) {
        datagramCount++
        if (length < HEADER_SIZE || length > data.size) return
        val b = ByteBuffer.wrap(data, 0, length).order(ByteOrder.BIG_ENDIAN)
        if ((b.short.toInt() and 0xffff) != MAGIC || b.get().toInt() != VERSION) return
        val sessionId = b.int
        val frameId = b.short.toInt() and 0xffff
        val timestamp = b.int.toLong() and 0xffffffffL
        val fragmentIndex = b.short.toInt() and 0xffff
        val fragmentCount = b.short.toInt() and 0xffff
        val payloadLength = b.short.toInt() and 0xffff

        // A PC that hasn't assigned us a session yet sends 0; once it has,
        // anything else on this port is somebody else's stream.
        val expected = expectedSessionId()
        if (expected != 0 && sessionId != 0 && sessionId != expected) return

        if (fragmentCount == 0 || payloadLength > MAX_FRAGMENT_PAYLOAD ||
            payloadLength != length - HEADER_SIZE) return
        val isParity = fragmentIndex >= fragmentCount
        if (isParity && (payloadLength != MAX_FRAGMENT_PAYLOAD ||
                    fragmentIndex - fragmentCount >= fecGroupCount(fragmentCount))) return
        // Strictly newer than the last completed frame. isNewer(a, a) is
        // true, so an equality check is needed on top of it: the PC sends a
        // frame's FEC parity *after* its data, and without this the parity
        // for a frame that just completed recreated it as an empty pending
        // frame. That ghost timed out 80ms later, fired a PLI, and did so
        // for every multi-fragment frame - an IDR storm, plus a reported
        // 50% "loss" that drove QoS to its floor.
        if (lastFrameId >= 0 && (frameId == lastFrameId || !isNewer(frameId, lastFrameId))) return

        val payload = data.copyOfRange(HEADER_SIZE, length)

        // Single-fragment frames are the common case with a small P-frame:
        // nothing to reassemble, nothing to protect.
        if (fragmentCount == 1 && !isParity) {
            windowExpectedFragments++
            windowReceivedFragments++
            complete(frameId, payload, timestamp)
            cleanup()
            return
        }

        val now = System.currentTimeMillis()
        var isNewFrame = false
        val frame = pending.computeIfAbsent(frameId) {
            isNewFrame = true
            Pending(timestamp, fragmentCount, now)
        }
        if (isNewFrame) windowExpectedFragments += fragmentCount
        if (frame.fragmentCount != fragmentCount) return  // Mismatched view of the frame.

        if (isParity) {
            val group = fragmentIndex - fragmentCount
            if (group < frame.parity.size && frame.parity[group] == null) {
                frame.parity[group] = payload
                tryRecover(frame, group)
            }
        } else if (frame.fragments[fragmentIndex] == null) {
            frame.fragments[fragmentIndex] = payload
            frame.received++
            windowReceivedFragments++
            // A late data fragment can complete a group whose parity is
            // already here, so re-check that group too.
            tryRecover(frame, fragmentIndex / FEC_GROUP_SIZE)
        }

        if (frame.received == frame.fragmentCount) {
            pending.remove(frameId)
            complete(frameId, assemble(frame), frame.timestampMs)
        }
        cleanup()
    }

    /**
     * Rebuilds the one missing fragment of an FEC group, if exactly one is
     * missing and its parity has arrived. XOR of every other member with the
     * parity is the missing member.
     */
    private fun tryRecover(frame: Pending, group: Int) {
        if (group < 0 || group >= frame.parity.size) return
        val parity = frame.parity[group] ?: return
        val begin = group * FEC_GROUP_SIZE
        val end = minOf(begin + FEC_GROUP_SIZE, protectedFragmentCount(frame.fragmentCount))
        if (end - begin < 2) return

        var missingIndex = -1
        for (index in begin until end) {
            if (frame.fragments[index] == null) {
                if (missingIndex >= 0) return  // Two holes: XOR can't fix that.
                missingIndex = index
            }
        }
        if (missingIndex < 0) return

        val recovered = parity.copyOf()
        for (index in begin until end) {
            if (index == missingIndex) continue
            val fragment = frame.fragments[index] ?: return
            for (byte in recovered.indices) {
                recovered[byte] = (recovered[byte].toInt() xor fragment[byte].toInt()).toByte()
            }
        }
        frame.fragments[missingIndex] = recovered
        frame.received++
        windowReceivedFragments++
        windowRecoveredFragments++
        totalRecovered++
    }

    private fun assemble(frame: Pending): ByteArray {
        val assembled = ByteArray(frame.fragments.sumOf { it?.size ?: 0 })
        var offset = 0
        frame.fragments.forEach { fragment ->
            if (fragment != null) {
                fragment.copyInto(assembled, offset)
                offset += fragment.size
            }
        }
        return assembled
    }

    private fun complete(frameId: Int, data: ByteArray, timestampMs: Long) {
        lastFrameId = frameId
        lastFrame = frameId
        completedFrameCount++
        windowCompletedFrames++
        onFrame(data, timestampMs, frameId)
    }

    private fun cleanup() {
        val now = System.currentTimeMillis()
        var discarded = 0
        pending.entries.removeIf { (_, frame) ->
            if (now - frame.firstSeenMs > FRAME_TIMEOUT_MS) {
                discarded++
                discardedFrameCount++
                windowDroppedFrames++
                true
            } else false
        }
        if (discarded > 0) {
            // The frame is gone for good, so every P-frame that references it
            // is now decoding against something we never had. Ask for a
            // keyframe rather than showing a spreading smear.
            Log.w(tag, "Discarded $discarded incomplete frame(s) - requesting keyframe")
            onFrameLost()
        }
    }

    /** Drains the one-second window the control channel reports to the PC. */
    fun consumeStats(): Stats {
        val expected = windowExpectedFragments
        val received = windowReceivedFragments
        val lossPermille = if (expected > 0) {
            (((expected - received).coerceAtLeast(0L) * 1000L) / expected).toInt()
        } else 0
        val stats = Stats(
            lossPermille = lossPermille,
            completedFps = windowCompletedFrames,
            droppedFrames = windowDroppedFrames,
            recoveredFragments = totalRecovered,
        )
        windowExpectedFragments = 0
        windowReceivedFragments = 0
        windowRecoveredFragments = 0
        windowCompletedFrames = 0
        windowDroppedFrames = 0
        return stats
    }

    fun stats(): String =
        "UDP=$datagramCount complete=$completedFrameCount dropped=$discardedFrameCount " +
            "fec=$totalRecovered last=$lastFrame pending=${pending.size}"

    /** Completed frames so far; the UI diffs this to show a live frame rate. */
    fun completedFrames(): Long = completedFrameCount

    fun reset() {
        pending.clear()
        lastFrameId = -1
    }

    private fun isNewer(a: Int, b: Int): Boolean = ((a - b) and 0xffff) < 0x8000

    companion object {
        private const val MAGIC = 0xabba
        private const val VERSION = 0x02
        private const val HEADER_SIZE = 19
        private const val MAX_FRAGMENT_PAYLOAD = 1280
        private const val FRAME_TIMEOUT_MS = 80L
        const val FEC_GROUP_SIZE = 8

        /** The frame's final fragment is short, so it is never FEC-protected. */
        fun protectedFragmentCount(fragmentCount: Int): Int =
            if (fragmentCount > 1) fragmentCount - 1 else 0

        fun fecGroupCount(fragmentCount: Int): Int {
            val protectedCount = protectedFragmentCount(fragmentCount)
            if (protectedCount < 2) return 0
            return (protectedCount + FEC_GROUP_SIZE - 1) / FEC_GROUP_SIZE
        }
    }
}
