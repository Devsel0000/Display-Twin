// File: app/src/main/java/com/example/androidpendisplay/UdpReceiver.kt
package com.example.androidpendisplay

import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.SocketException
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Video receive socket.
 *
 * Datagrams from anywhere other than [allowedSourceIp] are dropped: without
 * this, any device on the same network could push its own H.264 stream at
 * this port and the tablet would happily decode it. The address follows the
 * PC the app is configured for, so changing the IP (or discovering a new
 * one) keeps working.
 */
class UdpReceiver(
    private val port: Int,
    allowedSourceIp: String? = null,
    private val onDataReceived: (ByteArray, Int) -> Unit
) {
    private var socket: DatagramSocket? = null
    private val isRunning = AtomicBoolean(false)
    private var receiveJob: Job? = null
    private val TAG = "UdpReceiver"

    @Volatile private var allowedSource: String? = allowedSourceIp
    @Volatile private var rejectedFromOtherSource = 0L

    /** Follows the configured PC address; null accepts any sender. */
    fun setAllowedSource(ip: String?) {
        allowedSource = ip
    }

    fun rejectedCount(): Long = rejectedFromOtherSource

    fun start(): Boolean {
        if (isRunning.getAndSet(true)) {
            Log.d(TAG, "Already running")
            return false
        }

        return try {
            socket = DatagramSocket(port).apply {
                soTimeout = 0
                // Matches the PC's send buffer. A full frame arrives as a
                // burst of datagrams, and a smaller buffer drops the tail of
                // the burst inside the kernel where nothing can see it.
                receiveBufferSize = 4 * 1024 * 1024
            }

            Log.d(TAG, "UDP receiver started on port $port (source=${allowedSource ?: "any"})")

            receiveJob = CoroutineScope(Dispatchers.IO).launch {
                val buffer = ByteArray(64 * 1024)
                val packet = DatagramPacket(buffer, buffer.size)

                while (isRunning.get() && isActive) {
                    try {
                        packet.setData(buffer, 0, buffer.size)
                        socket?.receive(packet)
                        if (packet.length <= 0) continue

                        val expected = allowedSource
                        if (expected != null && packet.address?.hostAddress != expected) {
                            rejectedFromOtherSource++
                            continue
                        }
                        onDataReceived(packet.data.copyOf(packet.length), packet.length)
                    } catch (e: SocketException) {
                        if (isRunning.get()) {
                            Log.e(TAG, "Socket error", e)
                        }
                        break
                    } catch (e: Exception) {
                        Log.e(TAG, "Receive error", e)
                    }
                }
            }
            true
        } catch (e: Exception) {
            isRunning.set(false)
            Log.e(TAG, "Start failed", e)
            false
        }
    }

    fun stop() {
        Log.d(TAG, "Stopping UDP receiver")
        isRunning.set(false)
        receiveJob?.cancel()
        receiveJob = null
        socket?.close()
        socket = null
    }
}
