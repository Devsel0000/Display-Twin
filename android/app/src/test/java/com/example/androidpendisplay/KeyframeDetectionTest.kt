package com.example.androidpendisplay

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The keyframe gate in [DecoderPipeline] is only as good as this check: if
 * it misses an IDR the picture never recovers after a loss, and if it
 * false-positives on a P-frame the decoder is fed garbage.
 */
class KeyframeDetectionTest {

    private fun annexB(vararg nalTypes: Int): ByteArray {
        val out = ArrayList<Byte>()
        for (type in nalTypes) {
            out.add(0); out.add(0); out.add(0); out.add(1)
            out.add((0x60 or type).toByte())  // nal_ref_idc | type
            repeat(8) { out.add(0x11) }
        }
        return out.toByteArray()
    }

    @Test
    fun `IDR slice is detected`() {
        assertTrue(H264Decoder.isKeyframe(annexB(5)))
    }

    @Test
    fun `SPS PPS IDR sequence is detected`() {
        // What repeatSPSPPS=1 actually puts on the wire for a keyframe.
        assertTrue(H264Decoder.isKeyframe(annexB(7, 8, 5)))
    }

    @Test
    fun `P-frame only access unit is not a keyframe`() {
        assertFalse(H264Decoder.isKeyframe(annexB(1)))
    }

    @Test
    fun `three byte start codes are handled`() {
        val data = byteArrayOf(0, 0, 1, (0x65).toByte(), 0x11, 0x22, 0x33, 0x44, 0x55)
        assertTrue(H264Decoder.isKeyframe(data))
    }

    @Test
    fun `short buffers do not crash`() {
        assertFalse(H264Decoder.isKeyframe(ByteArray(0)))
        assertFalse(H264Decoder.isKeyframe(byteArrayOf(0, 0)))
    }
}
