package com.example.androidpendisplay

import android.util.Log
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

/**
 * Sits between the reassembler and [H264Decoder] and owns three policies
 * that used to be missing entirely - decoding happened inline on the UDP
 * receive thread, so a slow decode stalled the socket and the kernel dropped
 * the fragments that were arriving meanwhile:
 *
 *  - **Its own thread and a short queue.** Receiving never waits for
 *    decoding again.
 *  - **Keyframe gate.** After any loss, P-frames reference data we never
 *    had; feeding them to the decoder paints spreading garbage. They are
 *    dropped until an IDR arrives.
 *  - **Stale-frame drop.** A frame older than [FRAME_DROP_AGE_MS] is worth
 *    less than the latency it would add, so it is thrown away and a
 *    keyframe requested. This needs the clock sync from [ControlChannel];
 *    without an offset the age is unknown and the check is skipped rather
 *    than guessed.
 *
 * When the queue is full the *newest* frame is dropped, not the oldest: the
 * queued frames are the ones the following P-frames reference, so dropping
 * them would break more than it saves.
 */
class DecoderPipeline(
    private val decoder: H264Decoder,
    private val onNeedKeyframe: () -> Unit,
    private val pcClockMs: () -> Long?,
) {
    private class Frame(val data: ByteArray, val timestampMs: Long, val frameId: Int)

    private val queue = ArrayBlockingQueue<Frame>(QUEUE_CAPACITY)
    private val running = AtomicBoolean(false)
    private var worker: Thread? = null

    @Volatile private var waitingForKeyframe = true
    @Volatile private var droppedFrames = 0
    @Volatile private var reportedDrops = 0
    @Volatile private var decodedFrames = 0
    @Volatile private var decodeMsTotal = 0.0
    private val tag = "DecoderPipeline"

    fun start() {
        if (running.getAndSet(true)) return
        waitingForKeyframe = true
        worker = thread(name = "h264-decode", isDaemon = true) { loop() }
    }

    fun stop() {
        running.set(false)
        worker?.interrupt()
        worker = null
        queue.clear()
    }

    fun submit(data: ByteArray, timestampMs: Long, frameId: Int) {
        val keyframe = H264Decoder.isKeyframe(data)

        if (waitingForKeyframe) {
            if (!keyframe) {
                droppedFrames++
                return
            }
            waitingForKeyframe = false
        }

        // Age is measured against the PC's clock, which is only known once
        // the control channel has completed a sync. 32-bit arithmetic on
        // purpose: video timestamps are the low 32 bits of the PC's
        // monotonic millisecond counter, so truncating both sides makes the
        // subtraction wrap correctly.
        val pcNow = pcClockMs()
        if (pcNow != null) {
            val ageMs = pcNow.toInt() - timestampMs.toInt()
            if (ageMs > FRAME_DROP_AGE_MS) {
                droppedFrames++
                requestKeyframe("frame $frameId was $ageMs ms old")
                return
            }
        }

        if (!queue.offer(Frame(data, timestampMs, frameId))) {
            // The decoder is behind. Drop what just arrived - the frames
            // already queued are the ones everything after them references.
            droppedFrames++
            requestKeyframe("decoder queue full")
        }
    }

    fun queueLength(): Int = queue.size

    fun droppedFrames(): Int = droppedFrames

    /** Drains the drop count for the one-second status report. */
    fun consumeDropped(): Int {
        val dropped = droppedFrames - reportedDrops
        reportedDrops = droppedFrames
        return dropped
    }

    /** Average decode+render time over the last window, in tenths of a millisecond. */
    fun consumeDecodeMsX10(): Int {
        val frames = decodedFrames
        val total = decodeMsTotal
        decodedFrames = 0
        decodeMsTotal = 0.0
        if (frames == 0) return 0
        return ((total / frames) * 10.0).toInt().coerceIn(0, 65535)
    }

    private fun requestKeyframe(reason: String) {
        if (!waitingForKeyframe) Log.w(tag, "Requesting keyframe: $reason")
        waitingForKeyframe = true
        onNeedKeyframe()
    }

    private fun loop() {
        while (running.get()) {
            val frame = try {
                queue.poll(100, TimeUnit.MILLISECONDS)
            } catch (e: InterruptedException) {
                return
            } ?: continue

            val startNs = System.nanoTime()
            val ok = decoder.decode(frame.data, frame.timestampMs * 1000L)
            decodeMsTotal += (System.nanoTime() - startNs) / 1_000_000.0
            decodedFrames++

            if (!ok) {
                // MediaCodec threw. Flushing (or, failing that, a full
                // re-init) is the only way back - without this the codec
                // stayed wedged until the user restarted the app.
                Log.w(tag, "Decoder error - recovering")
                queue.clear()
                if (!decoder.recover()) {
                    Log.e(tag, "Decoder recovery failed")
                }
                requestKeyframe("decoder reset")
            }
        }
    }

    companion object {
        /** Four frames is ~66ms at 60fps: enough to absorb jitter, not enough to add visible lag. */
        const val QUEUE_CAPACITY = 4

        /**
         * Only a frame this late is dropped. Dropping any P-frame breaks the
         * reference chain and costs a keyframe, and under low-delay CBR a
         * keyframe is a visibly softer picture that then sharpens back up.
         * The spec's 50ms assumed a drop was free; in practice a Wi-Fi
         * hiccup delivered a frame 64ms late and the "fix" was a quality
         * pulse far more noticeable than showing that frame a little late.
         * The 4-frame queue already bounds how much latency can pile up, so
         * this is only for genuine stalls, where resyncing is worth a pulse.
         */
        const val FRAME_DROP_AGE_MS = 250
    }
}
