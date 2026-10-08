package com.example.androidpendisplay

import android.os.SystemClock
import android.util.Log
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

/**
 * The control channel (UDP 5002), which is everything the tablet and the PC
 * need to say to each other that isn't a video fragment or a pen sample:
 *
 *  - HELLO / WELCOME: the PC assigns a SessionID and, because it now knows
 *    where we are, starts sending video here without anyone typing an IP.
 *  - PING / PONG: NTP-style clock sync. Without it there is no way to tell
 *    how old an arriving frame is, so [InkOverlayView]'s hold time and the
 *    stale-frame policy were both guesswork.
 *  - PLI: "I had to throw a frame away, please send a fresh keyframe."
 *    Rate-limited, because a burst of loss produces a burst of PLIs and one
 *    IDR repairs all of them.
 *  - STATUS: our decode queue, loss rate and RTT once a second, which is
 *    what the PC's QoS decisions are made of.
 *  - PENACK: the PC acknowledging a pen DOWN/UP so [PenInputSender] can stop
 *    retransmitting it.
 *
 * The socket binds an ephemeral port; the PC always replies to whatever
 * address a request came from, so no fixed local port is needed.
 */
class ControlChannel(
    private val pcIp: String,
    private val pcPort: Int = 5002,
    private val statusProvider: () -> ClientStatus,
    private val onSession: (Int) -> Unit = {},
    private val onBusy: () -> Unit = {},
) {
    data class ClientStatus(
        val decodeQueueLength: Int,
        val lossPermille: Int,
        val completedFps: Int,
        val droppedFrames: Int,
        val decodeMsX10: Int,
    )

    private var socket: DatagramSocket? = null
    private var address: InetAddress? = null
    private val running = AtomicBoolean(false)
    private val touchExecutor = java.util.concurrent.Executors.newSingleThreadExecutor()
    private val tag = "ControlChannel"

    @Volatile var sessionId: Int = 0
        private set

    /** PC clock minus our clock, in milliseconds. Only meaningful once [hasClockOffset]. */
    @Volatile var clockOffsetMs: Long = 0L
        private set

    @Volatile var hasClockOffset: Boolean = false
        private set

    /** Round trip of the best (lowest-RTT) sample so far. */
    @Volatile var rttMs: Int = 0
        private set

    @Volatile private var lastPliMs = 0L
    @Volatile private var bestRttMs = Long.MAX_VALUE
    @Volatile private var lastSyncMs = 0L
    private var pliSent = 0L

    var onPenAck: ((Int) -> Unit)? = null

    fun start(): Boolean {
        if (running.getAndSet(true)) return false
        return try {
            address = InetAddress.getByName(pcIp)
            socket = DatagramSocket().apply { soTimeout = 500 }
            thread(name = "control-rx", isDaemon = true) { receiveLoop() }
            thread(name = "control-tx", isDaemon = true) { sendLoop() }
            true
        } catch (e: Exception) {
            Log.e(tag, "start failed", e)
            running.set(false)
            false
        }
    }

    fun stop() {
        running.set(false)
        touchExecutor.shutdown()
        socket?.close()
        socket = null
        sessionId = 0
        hasClockOffset = false
        bestRttMs = Long.MAX_VALUE
    }

    /** Estimated PC monotonic clock, in the same 32-bit domain as video timestamps. */
    fun pcNowMs(): Long = SystemClock.elapsedRealtime() + clockOffsetMs

    /**
     * Asks the PC for a fresh keyframe. Rate-limited to [PLI_MIN_INTERVAL_MS]:
     * losing ten fragments in a row is one problem, not ten, and answering
     * each with its own IDR would push a stream of large frames into a link
     * that is already dropping packets.
     */
    fun sendPli() {
        val now = SystemClock.elapsedRealtime()
        if (now - lastPliMs < PLI_MIN_INTERVAL_MS) return
        lastPliMs = now
        pliSent++
        send(buildPacket(TYPE_PLI, ByteArray(0)))
    }

    fun pliCount(): Long = pliSent

    /** Complete set of current fingers (id, x, y; x/y normalized 0..1). Empty = all up. */
    fun sendTouch(contacts: List<Triple<Int, Float, Float>>) {
        if (sessionId == 0) return
        val b = ByteBuffer.allocate(1 + 5 * contacts.size).order(ByteOrder.BIG_ENDIAN)
        b.put(contacts.size.toByte())
        for ((id, x, y) in contacts) {
            b.put(id.toByte())
            b.putShort((x.coerceIn(0f, 1f) * 32767f).toInt().toShort())
            b.putShort((y.coerceIn(0f, 1f) * 32767f).toInt().toShort())
        }
        // Called from the UI thread; socket sends must not run there.
        val packet = buildPacket(TYPE_TOUCH, b.array())
        touchExecutor.execute { send(packet) }
    }

    private fun sendLoop() {
        while (running.get()) {
            try {
                if (sessionId == 0) {
                    send(buildPacket(TYPE_HELLO, CLIENT_VERSION.toByteArray(Charsets.US_ASCII)))
                    Thread.sleep(500)
                    continue
                }

                // Clock sync: a burst of pings at first (the lowest-RTT
                // sample of the burst is the least distorted by queueing),
                // then a single ping a second to keep the session alive and
                // the offset fresh, with a fresh burst every 30s.
                val now = SystemClock.elapsedRealtime()
                if (!hasClockOffset || now - lastSyncMs >= CLOCK_RESYNC_MS) {
                    bestRttMs = Long.MAX_VALUE
                    repeat(CLOCK_SYNC_SAMPLES) {
                        sendPing()
                        Thread.sleep(30)
                    }
                    lastSyncMs = now
                } else {
                    sendPing()
                }
                sendStatus()
                Thread.sleep(1000)
            } catch (e: InterruptedException) {
                return
            } catch (e: Exception) {
                if (running.get()) Log.e(tag, "send loop error", e)
            }
        }
    }

    private fun sendPing() {
        val payload = ByteBuffer.allocate(8).order(ByteOrder.BIG_ENDIAN)
            .putLong(SystemClock.elapsedRealtime()).array()
        send(buildPacket(TYPE_PING, payload))
    }

    private fun sendStatus() {
        val status = statusProvider()
        val payload = ByteBuffer.allocate(STATUS_SIZE).order(ByteOrder.BIG_ENDIAN)
        payload.put(status.decodeQueueLength.coerceIn(0, 255).toByte())
        payload.put(0)
        payload.putShort(status.lossPermille.coerceIn(0, 65535).toShort())
        payload.putShort(status.completedFps.coerceIn(0, 65535).toShort())
        payload.putShort(status.droppedFrames.coerceIn(0, 65535).toShort())
        payload.putShort(status.decodeMsX10.coerceIn(0, 65535).toShort())
        payload.putShort(rttMs.coerceIn(0, 65535).toShort())
        send(buildPacket(TYPE_STATUS, payload.array()))
    }

    private fun receiveLoop() {
        val buffer = ByteArray(2048)
        val packet = DatagramPacket(buffer, buffer.size)
        while (running.get()) {
            try {
                socket?.receive(packet) ?: return
                handle(packet.data, packet.length)
            } catch (e: java.net.SocketTimeoutException) {
                // Normal: the channel is quiet between our own requests.
            } catch (e: Exception) {
                if (running.get()) Log.e(tag, "receive error", e)
                else return
            }
        }
    }

    private fun handle(data: ByteArray, length: Int) {
        if (length < HEADER_SIZE) return
        val b = ByteBuffer.wrap(data, 0, length).order(ByteOrder.BIG_ENDIAN)
        if ((b.short.toInt() and 0xffff) != MAGIC || b.get().toInt() != VERSION) return
        val session = b.int
        val type = b.get().toInt() and 0xff
        val payloadLength = b.short.toInt() and 0xffff
        if (payloadLength != length - HEADER_SIZE) return

        when (type) {
            TYPE_WELCOME -> {
                if (payloadLength < 4) return
                val assigned = b.int
                sessionId = assigned
                Log.d(tag, "WELCOME session=$assigned")
                onSession(assigned)
            }
            TYPE_BUSY -> {
                Log.w(tag, "PC is busy with another tablet")
                onBusy()
            }
            TYPE_PONG -> {
                if (payloadLength < 24) return
                // The PC stamps every reply with the session it currently
                // has for us. A mismatch means it no longer knows us - the
                // host was restarted - so go back to HELLO. Without this the
                // tablet kept pinging with a dead session forever and the PC
                // ran without one: no QoS input, and keyframes only from the
                // no-session fallback.
                if (sessionId != 0 && session != sessionId) {
                    Log.w(tag, "PC dropped our session - re-handshaking")
                    sessionId = 0
                    hasClockOffset = false
                    bestRttMs = Long.MAX_VALUE
                    return
                }
                val t1 = b.long
                val t2 = b.long
                val t3 = b.long
                val t4 = SystemClock.elapsedRealtime()
                val rtt = (t4 - t1) - (t3 - t2)
                // Keep the lowest-RTT sample: a long round trip means the
                // packet queued somewhere, and queueing is exactly what
                // skews the offset estimate.
                if (rtt in 0 until bestRttMs) {
                    bestRttMs = rtt
                    val offset = ((t2 - t1) + (t3 - t4)) / 2
                    clockOffsetMs = if (hasClockOffset) {
                        // Slew rather than jump: a step change would make a
                        // batch of in-flight frames look either ancient or
                        // from the future, and the stale-frame policy would
                        // act on it.
                        clockOffsetMs + (offset - clockOffsetMs) / 4
                    } else {
                        offset
                    }
                    hasClockOffset = true
                    rttMs = rtt.toInt()
                }
            }
            TYPE_PEN_ACK -> {
                // Only our own session's acknowledgements count; a stray one
                // would clear a retransmit we still need to make.
                if (payloadLength < 2 || (sessionId != 0 && session != sessionId)) return
                onPenAck?.invoke(b.short.toInt() and 0xffff)
            }
            else -> Unit
        }
    }

    private fun buildPacket(type: Int, payload: ByteArray): ByteArray {
        val buffer = ByteBuffer.allocate(HEADER_SIZE + payload.size).order(ByteOrder.BIG_ENDIAN)
        buffer.putShort(MAGIC.toShort())
        buffer.put(VERSION.toByte())
        buffer.putInt(sessionId)
        buffer.put(type.toByte())
        buffer.putShort(payload.size.toShort())
        buffer.put(payload)
        return buffer.array()
    }

    private fun send(bytes: ByteArray) {
        val destination = address ?: return
        try {
            socket?.send(DatagramPacket(bytes, bytes.size, destination, pcPort))
        } catch (e: Exception) {
            if (running.get()) Log.e(tag, "send failed", e)
        }
    }

    companion object {
        const val MAGIC = 0xabba
        const val VERSION = 0x02
        const val HEADER_SIZE = 10
        const val STATUS_SIZE = 12

        const val TYPE_PING = 0x01
        const val TYPE_PONG = 0x02
        const val TYPE_PLI = 0x11
        const val TYPE_HELLO = 0x20
        const val TYPE_WELCOME = 0x21
        const val TYPE_BUSY = 0x22
        const val TYPE_STATUS = 0x30
        const val TYPE_PEN_ACK = 0x40
        const val TYPE_TOUCH = 0x50

        const val PLI_MIN_INTERVAL_MS = 100L
        const val CLOCK_SYNC_SAMPLES = 8
        const val CLOCK_RESYNC_MS = 30_000L
        const val CLIENT_VERSION = "DisplayTwin-Android/4.2"
    }
}
