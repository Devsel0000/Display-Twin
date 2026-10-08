// File: app/src/main/java/com/example/androidpendisplay/H264Decoder.kt (수정 버전)
package com.example.androidpendisplay

import android.media.MediaCodec
import android.media.MediaCodecList
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer

class H264Decoder(
    private val surface: Surface?,
    private val onVideoSizeChanged: ((Int, Int) -> Unit)? = null
) {
    private var mediaCodec: MediaCodec? = null
    private var frameCounter = 0L
    private var inputCounter = 0L
    @Volatile private var lastError = "none"
    private val TAG = "H264Decoder"

    @Synchronized
    fun init(): Boolean {
        if (surface == null || !surface.isValid) {
            Log.e(TAG, "Surface is null or invalid")
            return false
        }

        return try {
            val format = MediaFormat.createVideoFormat(
                MediaFormat.MIMETYPE_VIDEO_AVC,
                1920,
                1080
            ).apply {
                setInteger(MediaFormat.KEY_FRAME_RATE, 60)
                // Realtime priority: tells the codec this is a live stream, not
                // file playback, so it schedules for latency over throughput.
                setInteger(MediaFormat.KEY_PRIORITY, 0)
                // Hint the expected decode rate. Combined with KEY_PRIORITY this
                // keeps the decoder clocked up instead of ramping lazily.
                setInteger(MediaFormat.KEY_OPERATING_RATE, 60)
            }

            val codec = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
            val caps = codec.codecInfo.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC)
            if (caps.isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_LowLatency)) {
                format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            } else {
                // Vendor fallback used by many pre-Android-11 decoders that
                // support low latency without advertising the feature.
                format.setInteger("vdec-lowlatency", 1)
            }

            Log.d(TAG, "Using decoder: ${codec.name}")

            mediaCodec = codec.apply {
                configure(format, surface, null, 0)
                start()
                setVideoScalingMode(MediaCodec.VIDEO_SCALING_MODE_SCALE_TO_FIT)
            }

            Log.d(TAG, "Decoder initialized successfully")
            true
        } catch (e: Exception) {
            Log.e(TAG, "Init failed", e)
            false
        }
    }

    /**
     * Flushes the codec, and if that fails tears it down and starts a new
     * one. Called by [DecoderPipeline] after a decode error - previously an
     * exception here left the codec wedged for the rest of the session.
     * Either way the caller must wait for a keyframe afterwards.
     */
    @Synchronized
    fun recover(): Boolean {
        val codec = mediaCodec
        if (codec != null) {
            try {
                codec.flush()
                codec.start()
                lastError = "recovered by flush"
                return true
            } catch (e: Exception) {
                Log.w(TAG, "Flush failed, re-creating the decoder", e)
            }
        }
        release()
        return init()
    }

    @Synchronized
    fun decode(data: ByteArray, timestampUs: Long): Boolean {
        val codec = mediaCodec ?: return false

        try {
            // ✅ 입력 버퍼 처리 (타임아웃 10ms)
            val inputIndex = codec.dequeueInputBuffer(10_000)
            if (inputIndex >= 0) {
                val inputBuffer = codec.getInputBuffer(inputIndex)
                if (inputBuffer != null) {
                    inputBuffer.clear()
                    if (data.size > inputBuffer.remaining()) return false
                    inputBuffer.put(data)
                    codec.queueInputBuffer(inputIndex, 0, data.size, timestampUs, 0)
                    inputCounter++
                    if (inputCounter == 1L || inputCounter % 30L == 0L || containsIdr(data)) {
                        Log.d(TAG, "Queued AU #$inputCounter size=${data.size} idr=${containsIdr(data)} bytes=${hexPrefix(data)} ptsUs=$timestampUs")
                    }
                }
            }

            // ✅ 출력 버퍼 처리 (타임아웃 10ms)
            val bufferInfo = MediaCodec.BufferInfo()
            var outputIndex = codec.dequeueOutputBuffer(bufferInfo, 10_000)
            while (true) {
                when {
                    outputIndex >= 0 -> {
                        codec.releaseOutputBuffer(outputIndex, true)
                        frameCounter++
                        if (frameCounter == 1L || frameCounter % 30L == 0L) {
                            Log.d(TAG, "Rendered frame #$frameCounter size=${bufferInfo.size} ptsUs=${bufferInfo.presentationTimeUs}")
                        }
                    }
                    outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        // The decoder resolves the real stream dimensions from the
                        // SPS, which may differ from the 1920x1080 hint passed at
                        // configure() time (e.g. a non-FHD PC monitor). Report it so
                        // the view can size its letterbox/touch-mapping correctly.
                        val format = codec.outputFormat
                        val width = format.getInteger(MediaFormat.KEY_WIDTH)
                        val height = format.getInteger(MediaFormat.KEY_HEIGHT)
                        Log.d(TAG, "Output format changed: ${width}x$height")
                        onVideoSizeChanged?.invoke(width, height)
                    }
                    else -> break // INFO_TRY_AGAIN_LATER or INFO_OUTPUT_BUFFERS_CHANGED
                }
                outputIndex = codec.dequeueOutputBuffer(bufferInfo, 0)
            }
        } catch (e: Exception) {
            lastError = e.javaClass.simpleName + ": " + (e.message ?: "")
            Log.e(TAG, "Decode error", e)
            return false
        }
        return true
    }

    @Synchronized
    fun getFrameCount(): Long = frameCounter

    fun stats(): String = "input=$inputCounter rendered=$frameCounter error=$lastError"

    private fun containsIdr(data: ByteArray): Boolean = isKeyframe(data)

    companion object {
        /**
         * True when this access unit contains an IDR slice.
         *
         * [DecoderPipeline] gates on it: after any loss every P-frame
         * references data that never arrived, so nothing may be decoded
         * until the next IDR - otherwise the decoder paints garbage that
         * spreads across the picture until the periodic keyframe.
         */
        fun isKeyframe(data: ByteArray): Boolean {
            for (i in 0 until data.size - 4) {
                val startCode = (data[i].toInt() == 0 && data[i + 1].toInt() == 0 &&
                        ((data[i + 2].toInt() == 1) ||
                                (data[i + 2].toInt() == 0 && data[i + 3].toInt() == 1)))
                if (!startCode) continue
                val headerIndex = if (data[i + 2].toInt() == 1) i + 3 else i + 4
                if (headerIndex < data.size && (data[headerIndex].toInt() and 0x1f) == 5) return true
            }
            // Some encoders use 4-byte length-prefixed NAL units instead of
            // Annex-B start codes. Accept both forms.
            var offset = 0
            while (offset + 4 <= data.size) {
                val nalLength = ((data[offset].toInt() and 0xff) shl 24) or
                        ((data[offset + 1].toInt() and 0xff) shl 16) or
                        ((data[offset + 2].toInt() and 0xff) shl 8) or
                        (data[offset + 3].toInt() and 0xff)
                if (nalLength <= 0 || nalLength > data.size - offset - 4) break
                if ((data[offset + 4].toInt() and 0x1f) == 5) return true
                offset += 4 + nalLength
            }
            return false
        }
    }

    private fun hexPrefix(data: ByteArray): String = data.take(16)
        .joinToString(separator = "") { "%02x".format(it.toInt() and 0xff) }

    @Synchronized
    fun release() {
        try {
            val codec = mediaCodec ?: return
            mediaCodec = null
            try {
                codec.stop()
            } catch (e: IllegalStateException) {
                Log.w(TAG, "Decoder was already stopped", e)
            }
            codec.release()
            Log.d(TAG, "Decoder released")
        } catch (e: Exception) {
            Log.e(TAG, "Release error", e)
        }
    }
}
