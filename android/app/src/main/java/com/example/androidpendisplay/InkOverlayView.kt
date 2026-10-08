package com.example.androidpendisplay

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.PixelFormat
import android.graphics.PorterDuff
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.AttributeSet
import android.view.SurfaceHolder
import android.view.SurfaceView

/**
 * Purely visual overlay that draws the pen stroke locally the instant a touch
 * sample arrives, before the round trip to the PC and back as decoded video
 * has any chance to catch up. Points are held for the hold window and then
 * dropped (with a short fade), on the assumption that by then the real
 * stroke has arrived in the video and superseded the prediction.
 *
 * The hold window used to be a fixed 150ms guess because nothing measured
 * the round trip. It is now driven by [setRoundTripMs] from the control
 * channel's PING/PONG plus the tablet's own decode time. Too short and the
 * ghost stroke disappears before the real one arrives (a visible gap); too
 * long and it lingers over the real ink, so it stays clamped to a sane
 * range in case a single RTT sample is wild.
 *
 * Rendering-only: never handles touch (isClickable/isFocusable are left
 * false) so events pass through to [PenSurfaceView] beneath it.
 */
class InkOverlayView(context: Context, attrs: AttributeSet?) :
    SurfaceView(context, attrs), SurfaceHolder.Callback {

    private data class InkPoint(
        val x: Float,
        val y: Float,
        val pressure: Float,
        val tMs: Long,
        val startsStroke: Boolean
    )

    // All access is from the UI thread: onPenDown/Move/Up/Cancel are called
    // synchronously from PenSurfaceView.onTouchEvent, and the redraw loop
    // runs on a main-looper Handler.
    private val points = ArrayList<InkPoint>()

    /**
     * How long a predicted point stays on screen. This is meant to be the
     * time it takes for the real stroke to come back as decoded video, and
     * it used to be a fixed guess because nothing measured that. With clock
     * sync in place, [setRoundTripMs] derives it from the actual round trip
     * plus the tablet's own decode time.
     */
    @Volatile private var holdMs = DEFAULT_HOLD_MS

    /**
     * Sets the hold time from the measured round trip. Too short and the
     * ghost stroke vanishes before the real one arrives (a visible gap);
     * too long and it lingers on top of the real ink.
     */
    fun setRoundTripMs(rttMs: Int, decodeMs: Float) {
        if (rttMs <= 0) return
        holdMs = (rttMs + decodeMs.toLong() + RENDER_MARGIN_MS)
            .coerceIn(MIN_HOLD_MS, MAX_HOLD_MS)
    }

    private var surfaceReady = false
    private var videoWidth = DEFAULT_VIDEO_WIDTH
    private var videoHeight = DEFAULT_VIDEO_HEIGHT

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.BLACK
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
    }

    private val redrawHandler = Handler(Looper.getMainLooper())
    private var redrawScheduled = false
    private val redrawRunnable = Runnable {
        redrawScheduled = false
        draw()
        if (points.isNotEmpty()) scheduleRedraw()
    }

    init {
        // PenSurfaceView (the video) also uses setZOrderOnTop(true). Two
        // setZOrderOnTop surfaces don't have a documented relative order,
        // but in practice the one added later in the view hierarchy - this
        // one - lands on top, which is what we rely on here. The
        // "documented" pairing (video=setZOrderMediaOverlay,
        // overlay=setZOrderOnTop) was tried first and broke video display
        // entirely - see PenSurfaceView's comment.
        setZOrderOnTop(true)
        holder.setFormat(PixelFormat.TRANSLUCENT)
        holder.addCallback(this)
        isClickable = false
        isFocusable = false
    }

    /** Keep in sync with PenSurfaceView.setVideoSize() - same source, same aspect lock. */
    fun setVideoSize(width: Int, height: Int) {
        if (width <= 0 || height <= 0) return
        if (width == videoWidth && height == videoHeight) return
        videoWidth = width
        videoHeight = height
        requestLayout()
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        // Mirrors PenSurfaceView.onMeasure() exactly so both views land on
        // identical pixel bounds - the ghost stroke must line up 1:1 with
        // where PenSurfaceView thinks the video is.
        val maxWidth = MeasureSpec.getSize(widthMeasureSpec)
        val maxHeight = MeasureSpec.getSize(heightMeasureSpec)
        val videoAspect = videoWidth.toFloat() / videoHeight.toFloat()
        var w = maxWidth
        var h = (w / videoAspect).toInt()
        if (h > maxHeight) {
            h = maxHeight
            w = (h * videoAspect).toInt()
        }
        setMeasuredDimension(w, h)
    }

    /** [nx]/[ny] are normalized 0..1, same coordinate space PenSurfaceView sends over the wire. */
    fun onPenDown(nx: Float, ny: Float, pressure: Float) {
        points.add(InkPoint(nx, ny, pressure, SystemClock.uptimeMillis(), startsStroke = true))
        scheduleRedraw()
    }

    fun onPenMove(nx: Float, ny: Float, pressure: Float) {
        points.add(InkPoint(nx, ny, pressure, SystemClock.uptimeMillis(), startsStroke = false))
        scheduleRedraw()
    }

    fun onPenUp() {
        // Nothing to add - the tail of the stroke just ages out on its own
        // schedule, fading as the real frame is expected to have arrived.
        scheduleRedraw()
    }

    fun onPenCancel() {
        points.clear()
        scheduleRedraw()
    }

    private fun scheduleRedraw() {
        if (redrawScheduled) return
        redrawScheduled = true
        redrawHandler.postDelayed(redrawRunnable, FRAME_INTERVAL_MS)
    }

    private fun draw() {
        if (!surfaceReady) return
        val w = width
        val h = height
        if (w <= 0 || h <= 0) return

        val now = SystemClock.uptimeMillis()
        val holdMs = this.holdMs
        points.removeAll { now - it.tMs > holdMs }

        val canvas: Canvas = try {
            holder.lockCanvas()
        } catch (e: Exception) {
            null
        } ?: return
        try {
            canvas.drawColor(Color.TRANSPARENT, PorterDuff.Mode.CLEAR)
            var prev: InkPoint? = null
            for (p in points) {
                if (p.startsStroke) prev = null
                val age = now - p.tMs
                val fade = ((holdMs - age).toFloat() / FADE_MS.toFloat()).coerceIn(0f, 1f)
                paint.alpha = (fade * 255).toInt()
                paint.strokeWidth = BASE_WIDTH_PX * (0.35f + 0.65f * p.pressure.coerceIn(0f, 1f))

                val before = prev
                if (before != null) {
                    canvas.drawLine(before.x * w, before.y * h, p.x * w, p.y * h, paint)
                } else {
                    canvas.drawCircle(p.x * w, p.y * h, paint.strokeWidth / 2f, paint)
                }
                prev = p
            }
        } finally {
            holder.unlockCanvasAndPost(canvas)
        }
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        surfaceReady = true
        draw()
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        draw()
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        surfaceReady = false
    }

    companion object {
        private const val DEFAULT_VIDEO_WIDTH = 1920
        private const val DEFAULT_VIDEO_HEIGHT = 1080

        // Starting point only - setRoundTripMs() replaces it with a figure
        // derived from the measured round trip as soon as the control
        // channel has one.
        private const val DEFAULT_HOLD_MS = 150L
        private const val MIN_HOLD_MS = 60L
        private const val MAX_HOLD_MS = 400L
        // Capture, encode and compositing that the round trip doesn't cover.
        private const val RENDER_MARGIN_MS = 25L
        // Fade-out window at the tail end of the hold, so removal isn't a hard pop.
        private const val FADE_MS = 60L
        private const val FRAME_INTERVAL_MS = 16L
        private const val BASE_WIDTH_PX = 10f
    }
}
