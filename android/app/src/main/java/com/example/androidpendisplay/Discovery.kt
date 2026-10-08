package com.example.androidpendisplay

import android.util.Log
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.SocketTimeoutException

/**
 * Finds hosts on the local network instead of making the user read an IP off
 * the PC screen and type it in. Broadcasts a one-line request on UDP 9999 and
 * collects the replies for a couple of seconds.
 *
 * Broadcast doesn't cross subnets, which is exactly right here - the PC and
 * the tablet have to be on the same link for any of this to work anyway.
 */
object Discovery {

    data class Host(val ip: String, val name: String, val version: String)

    private const val PORT = 9999
    private const val REQUEST = "DTWIN-DISCOVER/1"
    private const val OFFER_PREFIX = "DTWIN-OFFER/1|"
    private const val LISTEN_MS = 2000
    private const val TAG = "Discovery"

    /**
     * Blocking - call from a background thread. Returns every distinct host
     * that answered, in the order they replied.
     */
    fun search(timeoutMs: Int = LISTEN_MS): List<Host> {
        val found = LinkedHashMap<String, Host>()
        var socket: DatagramSocket? = null
        try {
            socket = DatagramSocket().apply {
                broadcast = true
                soTimeout = 250
            }
            val payload = REQUEST.toByteArray(Charsets.US_ASCII)
            val broadcastAddress = InetAddress.getByName("255.255.255.255")
            socket.send(DatagramPacket(payload, payload.size, broadcastAddress, PORT))

            val buffer = ByteArray(512)
            val deadline = System.currentTimeMillis() + timeoutMs
            while (System.currentTimeMillis() < deadline) {
                val reply = DatagramPacket(buffer, buffer.size)
                try {
                    socket.receive(reply)
                } catch (e: SocketTimeoutException) {
                    continue
                }
                val text = String(reply.data, 0, reply.length, Charsets.US_ASCII)
                if (!text.startsWith(OFFER_PREFIX)) continue
                val parts = text.removePrefix(OFFER_PREFIX).split("|")
                val ip = reply.address.hostAddress ?: continue
                found[ip] = Host(
                    ip = ip,
                    name = parts.getOrNull(0)?.ifBlank { ip } ?: ip,
                    version = parts.getOrNull(1).orEmpty(),
                )
            }
        } catch (e: Exception) {
            Log.e(TAG, "search failed", e)
        } finally {
            socket?.close()
        }
        return found.values.toList()
    }
}
