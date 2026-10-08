package com.example.androidpendisplay

import android.os.SystemClock
import android.util.Log
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import kotlin.math.roundToInt

/**
 * Sends pen samples to the PC on UDP 5001.
 *
 * Two reliability rules, from the spec's split of "pen accuracy matters more
 * than pen latency" for state changes only:
 *
 *  - **MOVE is fire-and-forget.** A retransmitted MOVE would arrive after
 *    the stroke has already moved on, and the PC interpolates across a
 *    sequence gap anyway.
 *  - **DOWN / UP / CANCEL are acknowledged and retransmitted.** Losing one
 *    of these is visible: a missing DOWN eats the start of a stroke, a
 *    missing UP leaves the tip pressed until the PC's 3-second timeout.
 *
 * There is deliberately no rate limit here. There used to be one (2ms), and
 * because [PenSurfaceView] fans out MotionEvent's batched historical samples
 * in a tight loop, it silently threw away every historical sample but the
 * first - the exact samples that fan-out exists to preserve.
 */
class PenInputSender(
    private val ip: String,
    private val port: Int,
    private val sessionIdProvider: () -> Int = { 0 },
    private val rttMsProvider: () -> Int = { 0 },
) {
    private class PendingAck(val bytes: ByteArray, var sentAtMs: Long, var attempts: Int)

    private var socket: DatagramSocket? = null
    private var address: InetAddress? = null
    private var sequence = 0
    private val sendExecutor = Executors.newSingleThreadExecutor()
    private val retryExecutor = Executors.newSingleThreadScheduledExecutor()
    private val pendingAcks = ConcurrentHashMap<Int, PendingAck>()
    @Volatile private var retransmits = 0L
    private val tag = "PenInputSender"

    // Blocking: does DNS resolution + socket creation. Must only be called
    // from the sendExecutor background thread, never from the UI thread -
    // InetAddress.getByName() can stall for seconds on a network hiccup and
    // that used to freeze pen input handling (ANR risk).
    private fun connectBlocking(): Boolean = try {
        address = InetAddress.getByName(ip)
        socket = DatagramSocket().apply {
            reuseAddress = true
            sendBufferSize = 4 * 1024 * 1024
        }
        Log.d(tag, "Connected to $ip:$port")
        true
    } catch (e: Exception) {
        Log.e(tag, "Connect failed", e)
        false
    }

    /** Kicks off the initial connection in the background; never blocks the caller. */
    fun connect(onResult: ((Boolean) -> Unit)? = null) {
        sendExecutor.execute {
            val ok = connectBlocking()
            onResult?.invoke(ok)
        }
        retryExecutor.scheduleWithFixedDelay(
            { retransmitPending() }, RETRY_TICK_MS, RETRY_TICK_MS, TimeUnit.MILLISECONDS
        )
    }

    /** Called by the control channel when the PC acknowledges a state change. */
    fun onAck(sequence: Int) {
        pendingAcks.remove(sequence)
    }

    fun retransmitCount(): Long = retransmits

    fun sendPenData(
        action: Int,
        x: Float,
        y: Float,
        pressure: Float,
        tiltX: Float,
        tiltY: Float,
        stylus: Boolean,
        eraser: Boolean = false,
        palmRejected: Boolean = false,
        upHasLastMove: Boolean = false,
        timestampMs: Long
    ) {
        val sequenceNumber = sequence++ and 0xffff
        val packet = ByteBuffer.allocate(PACKET_SIZE).order(ByteOrder.BIG_ENDIAN)
        packet.putShort(MAGIC.toShort())
        packet.put(VERSION)
        packet.putInt(sessionIdProvider())
        packet.putShort(sequenceNumber.toShort())
        packet.put(action.toByte())
        var flags = 0
        if (eraser) flags = flags or FLAG_ERASER
        if (palmRejected) flags = flags or FLAG_PALM_REJECTION
        if (stylus) flags = flags or FLAG_STYLUS
        if (upHasLastMove) flags = flags or FLAG_UP_HAS_LAST_MOVE
        packet.put(flags.toByte())
        packet.putShort((pressure.coerceIn(0f, 1f) * 4095f).roundToInt().coerceAtLeast(1).toShort())
        packet.put(tiltByte(tiltX))
        packet.put(tiltByte(tiltY))
        packet.putShort((x.coerceIn(0f, 1f) * 32767f).roundToInt().toShort())
        packet.putShort((y.coerceIn(0f, 1f) * 32767f).roundToInt().toShort())
        packet.putInt(timestampMs.toInt())
        val bytes = packet.array().copyOf(packet.position())

        if (action != ACTION_MOVE) {
            pendingAcks[sequenceNumber] = PendingAck(bytes, SystemClock.elapsedRealtime(), 0)
        }
        transmit(bytes)
    }

    private fun transmit(bytes: ByteArray) {
        // All network I/O - including the lazy (re)connect - happens on the
        // single-thread executor so the UI/touch thread never blocks.
        sendExecutor.execute {
            try {
                if (socket == null || address == null) {
                    if (!connectBlocking()) return@execute
                }
                val destination = address ?: return@execute
                socket?.send(DatagramPacket(bytes, bytes.size, destination, port))
            } catch (e: Exception) {
                Log.e(tag, "Async send error", e)
                socket?.close()
                socket = null
            }
        }
    }

    /**
     * Resends state changes the PC hasn't acknowledged. The timeout follows
     * the measured round trip when the control channel has one, with a floor
     * so a fast LAN doesn't produce a retransmit storm.
     */
    private fun retransmitPending() {
        if (pendingAcks.isEmpty()) return
        val now = SystemClock.elapsedRealtime()
        val timeout = (rttMsProvider() * 2).coerceAtLeast(MIN_ACK_TIMEOUT_MS)
        val iterator = pendingAcks.entries.iterator()
        while (iterator.hasNext()) {
            val entry = iterator.next()
            val pending = entry.value
            if (now - pending.sentAtMs < timeout) continue
            if (pending.attempts >= MAX_ATTEMPTS) {
                Log.w(tag, "Giving up on pen state change seq=${entry.key}")
                iterator.remove()
                continue
            }
            pending.attempts++
            pending.sentAtMs = now
            retransmits++
            transmit(pending.bytes)
        }
    }

    private fun tiltByte(radians: Float): Byte {
        val degrees = (radians * 180f / Math.PI.toFloat()).coerceIn(-90f, 90f)
        return (degrees + 90f).roundToInt().coerceIn(0, 180).toByte()
    }

    fun close() {
        retryExecutor.shutdownNow()
        sendExecutor.shutdownNow()
        pendingAcks.clear()
        socket?.close()
        socket = null
        address = null
    }

    companion object {
        private const val MAGIC = 0xABBA
        private const val VERSION = 0x02.toByte()
        private const val PACKET_SIZE = 23
        private const val ACTION_MOVE = 1
        private const val FLAG_ERASER = 1
        private const val FLAG_PALM_REJECTION = 1 shl 1
        private const val FLAG_STYLUS = 1 shl 2
        private const val FLAG_UP_HAS_LAST_MOVE = 1 shl 3

        private const val RETRY_TICK_MS = 20L
        private const val MIN_ACK_TIMEOUT_MS = 40
        private const val MAX_ATTEMPTS = 4
    }
}
