// File: app/src/main/java/com/example/androidpendisplay/NetworkUtils.kt
package com.example.androidpendisplay

import android.content.Context
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.net.wifi.WifiManager
import android.util.Log
import java.net.InetAddress
import java.net.NetworkInterface
import java.util.*

object NetworkUtils {
    private val TAG = "NetworkUtils"
    const val DEFAULT_PC_IP = "192.168.0.16"

    fun getLocalIpAddress(): String {
        var fallback: String? = null
        return try {
            val interfaces = NetworkInterface.getNetworkInterfaces()
            while (interfaces.hasMoreElements()) {
                val iface = interfaces.nextElement()
                if (!iface.isUp || iface.isLoopback) continue
                val preferred = iface.name.lowercase().contains("wlan") ||
                        iface.name.lowercase().contains("rndis") ||
                        iface.name.lowercase().contains("usb") ||
                        iface.name.lowercase().contains("eth")
                val addresses = iface.inetAddresses
                while (addresses.hasMoreElements()) {
                    val address = addresses.nextElement()
                    val ip = address.hostAddress ?: continue
                    if (address is java.net.Inet4Address && !address.isLoopbackAddress) {
                        if (preferred) return ip
                        if (fallback == null) fallback = ip
                    }
                }
            }
            fallback ?: "Unavailable"
        } catch (e: Exception) {
            Log.e(TAG, "Failed to get local IP", e)
            "Unavailable"
        }
    }

    /**
     * USB 테더링 IP 주소를 자동으로 찾습니다.
     */
    fun getUsbTetheringIp(): String? {
        try {
            val interfaces = NetworkInterface.getNetworkInterfaces()
            while (interfaces.hasMoreElements()) {
                val iface = interfaces.nextElement()
                
                // USB 테더링 인터페이스 이름 패턴
                val name = iface.name.lowercase()
                if (name.contains("rndis") || 
                    name.contains("usb") || 
                    name.contains("eth") ||
                    name.contains("rmnet")) {
                    
                    val addresses = iface.inetAddresses
                    while (addresses.hasMoreElements()) {
                        val addr = addresses.nextElement()
                        if (!addr.isLoopbackAddress && addr is InetAddress) {
                            val ip = addr.hostAddress ?: continue
                            // 로컬 IP 범위 확인 (192.168.x.x)
                            if (ip.startsWith("192.168.")) {
                                Log.d(TAG, "Found USB tethering IP: $ip")
                                return ip
                            }
                        }
                    }
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to get USB tethering IP", e)
        }
        return null
    }

    /**
     * 현재 연결된 네트워크가 USB 테더링인지 확인합니다.
     */
    fun isUsbTetheringConnected(context: Context): Boolean {
        val connectivityManager = context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val network = connectivityManager.activeNetwork ?: return false
        val capabilities = connectivityManager.getNetworkCapabilities(network) ?: return false
        
        // USB 테더링은 일반적으로 WiFi 또는 Ethernet으로 표시됨
        return capabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) ||
               capabilities.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET)
    }

    /**
     * 네트워크 연결 상태 확인 (ping 대신 간단한 확인)
     */
    fun isNetworkAvailable(context: Context): Boolean {
        val connectivityManager = context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val network = connectivityManager.activeNetwork ?: return false
        val capabilities = connectivityManager.getNetworkCapabilities(network) ?: return false
        return capabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
    }
}
