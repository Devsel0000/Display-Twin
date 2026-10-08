package com.example.androidpendisplay

import android.content.Context
import android.util.AttributeSet
import android.util.Log
import android.view.MotionEvent
import android.graphics.PixelFormat
import android.graphics.RectF
import android.view.SurfaceView
import kotlin.math.cos
import kotlin.math.sin

class PenSurfaceView(context: Context, attrs: AttributeSet?) : SurfaceView(context, attrs) {
    private var penSender: PenInputSender? = null
    private var inkOverlay: InkOverlayView? = null
    private val tag = "PenSurfaceView"
    private var videoRect = RectF()
    private var lastX = 0.5f
    private var lastY = 0.5f
    private var hasLastPoint = false
    /** Wired to ControlChannel.sendTouch by MainActivity. */
    var touchSink: ((List<Triple<Int, Float, Float>>) -> Unit)? = null
    private var touchPending = false
    private var touchSwallow = false
    private var touchStartX = 0f
    private var touchStartY = 0f
    private var lastContacts: List<Triple<Int, Float, Float>> = emptyList()
    private var videoWidth = DEFAULT_VIDEO_WIDTH
    private var videoHeight = DEFAULT_VIDEO_HEIGHT

    init {
        // Rules out a composition bug that kept the decoded video from
        // showing at all - do not swap this for setZOrderMediaOverlay(),
        // that was tried to make room for InkOverlayView above it and broke
        // video display entirely. InkOverlayView instead also uses
        // setZOrderOnTop(true) and relies on view-hierarchy add order
        // (added after this one) to land on top - undocumented, but it is
        // what actually works, whereas the "documented" media-overlay
        // pairing did not.
        setZOrderOnTop(true)
        holder.setFormat(PixelFormat.OPAQUE)
        isFocusable = true
        isFocusableInTouchMode = true
        isClickable = true
        setWillNotDraw(false)
    }

    fun setPenSender(sender: PenInputSender?) {
        penSender = sender
    }

    /** Local ink prediction target - see InkOverlayView for why this exists. */
    fun setInkOverlay(overlay: InkOverlayView?) {
        inkOverlay = overlay
    }

    /**
     * The PC captures at its actual monitor resolution, which may not be
     * 1920x1080. Call this once the decoder reports the real stream size
     * (MediaCodec's INFO_OUTPUT_FORMAT_CHANGED) so this view resizes itself
     * to the video's real aspect ratio instead of being stretched to fill
     * the whole (differently-shaped) tablet screen.
     */
    fun setVideoSize(width: Int, height: Int) {
        if (width <= 0 || height <= 0) return
        if (width == videoWidth && height == videoHeight) return
        videoWidth = width
        videoHeight = height
        requestLayout()
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        // Constrain this view to the largest rect that preserves the video's
        // aspect ratio within the space the parent gives us, instead of
        // filling match_parent and letting MediaCodec's SCALE_TO_FIT stretch
        // the picture. That way the Surface's actual pixel bounds are the
        // video's real display area, so touch coordinates and what's on
        // screen are guaranteed to line up (no separate letterbox math
        // needed for the video itself).
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

    /**
     * Fingers are forwarded as raw touch contacts (max 2); the PC injects them
     * as a Windows touch device, so tap / long-press / drag / two-finger
     * scroll, pinch, rotate and two-finger tap are Windows' own. Every event
     * sends the full set of fingers still down; a heartbeat repeats it so a
     * held finger (long-press) isn't taken for a lost one.
     */
    private fun handleTouch(event: MotionEvent) {
        val ending = event.actionMasked == MotionEvent.ACTION_UP || event.actionMasked == MotionEvent.ACTION_CANCEL
        val lifted = if (event.actionMasked == MotionEvent.ACTION_POINTER_UP) event.actionIndex else -1
        var contacts: List<Triple<Int, Float, Float>> = emptyList()
        if (!ending) {
            val list = ArrayList<Triple<Int, Float, Float>>(2)
            for (i in 0 until event.pointerCount) {
                if (i == lifted || list.size >= 2) continue
                list.add(Triple(event.getPointerId(i), event.getX(i) / width, event.getY(i) / height))
            }
            contacts = list
        }
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                // Hold the first finger back: if a second one lands within
                // TOUCH_HOLD_MS this is a two-finger gesture, and sending the
                // first alone would already have started a stroke in Paint.
                touchSwallow = false
                touchPending = true
                touchStartX = event.x
                touchStartY = event.y
                lastContacts = contacts
                postDelayed(flushPending, TOUCH_HOLD_MS)
                return
            }
            MotionEvent.ACTION_POINTER_DOWN -> {
                touchPending = false
                removeCallbacks(flushPending)
            }
            MotionEvent.ACTION_POINTER_UP -> {
                // A two-finger gesture is over once one finger lifts; the
                // other must not carry on as a one-finger drag.
                touchSwallow = true
                contacts = emptyList()
            }
            MotionEvent.ACTION_MOVE -> if (touchPending) {
                lastContacts = contacts
                val dp = resources.displayMetrics.density
                if (Math.hypot((event.x - touchStartX).toDouble(), (event.y - touchStartY).toDouble()) > TOUCH_SLOP_DP * dp) {
                    removeCallbacks(flushPending)
                    flushPending.run()
                }
                return
            }
            MotionEvent.ACTION_UP -> if (touchPending) {
                // Quick tap: deliver the held DOWN, then the UP.
                removeCallbacks(flushPending)
                flushPending.run()
            }
        }
        if (touchPending && ending) { touchPending = false; removeCallbacks(flushPending) }
        if (touchSwallow) contacts = emptyList()
        emitTouch(contacts)
    }

    private val flushPending = Runnable {
        if (touchPending) {
            touchPending = false
            emitTouch(lastContacts)
        }
    }

    private fun emitTouch(contacts: List<Triple<Int, Float, Float>>) {
        lastContacts = contacts
        touchSink?.invoke(contacts)
        removeCallbacks(touchHeartbeat)
        if (contacts.isNotEmpty()) postDelayed(touchHeartbeat, TOUCH_HEARTBEAT_MS)
    }

    private val touchHeartbeat = object : Runnable {
        override fun run() {
            touchSink?.invoke(lastContacts)
            postDelayed(this, TOUCH_HEARTBEAT_MS)
        }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        val pointerIndex = event.actionIndex.coerceIn(0, (event.pointerCount - 1).coerceAtLeast(0))
        val toolType = if (event.pointerCount > 0) event.getToolType(pointerIndex) else MotionEvent.TOOL_TYPE_UNKNOWN
        if (toolType != MotionEvent.TOOL_TYPE_STYLUS && toolType != MotionEvent.TOOL_TYPE_FINGER) {
            return super.onTouchEvent(event)
        }

        // Palm rejection: while the stylus is on the glass, the hand resting
        // beside it registers as extra finger pointers. Swallow those rather
        // than drawing with them, and mark the stylus packets so the PC log
        // shows that rejection happened.
        var stylusPresent = false
        for (index in 0 until event.pointerCount) {
            if (event.getToolType(index) == MotionEvent.TOOL_TYPE_STYLUS) {
                stylusPresent = true
                break
            }
        }
        if (stylusPresent && lastContacts.isNotEmpty()) {
            // Pen arrived while fingers were down: lift them on the PC.
            lastContacts = emptyList()
            removeCallbacks(touchHeartbeat)
            touchSink?.invoke(lastContacts)
        }
        if (toolType == MotionEvent.TOOL_TYPE_FINGER) {
            if (!stylusPresent) handleTouch(event)
            return true
        }

        handlePenEvent(event, pointerIndex, toolType, palmRejected = stylusPresent && event.pointerCount > 1)
        return true
    }

    private fun handlePenEvent(event: MotionEvent, pointerIndex: Int, toolType: Int, palmRejected: Boolean) {
        // No rate limit. A digitizer tops out around 240Hz and a pen packet
        // is 23 bytes, so throttling saves nothing worth having - and the
        // 2ms gate that used to sit here dropped whole batched events,
        // historical samples included.
        val action = when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> 0
            MotionEvent.ACTION_MOVE -> 1
            MotionEvent.ACTION_UP -> 2
            MotionEvent.ACTION_CANCEL -> 3
            else -> return
        }

        // Fast strokes can batch multiple samples into one MOVE callback.
        // Forward the historical samples too so the PC side doesn't lose
        // intermediate points and render a choppier line than what was drawn.
        if (action == 1) {
            for (i in 0 until event.historySize) {
                sendSample(
                    action = 1,
                    x = event.getHistoricalX(pointerIndex, i),
                    y = event.getHistoricalY(pointerIndex, i),
                    pressure = event.getHistoricalPressure(pointerIndex, i),
                    tilt = event.getHistoricalAxisValue(MotionEvent.AXIS_TILT, pointerIndex, i),
                    orientation = event.getHistoricalAxisValue(MotionEvent.AXIS_ORIENTATION, pointerIndex, i),
                    toolType = toolType,
                    palmRejected = palmRejected,
                    upHasLastMove = false,
                    timestampMs = event.getHistoricalEventTime(i)
                )
            }
        }

        sendSample(
            action = action,
            x = event.getX(pointerIndex),
            y = event.getY(pointerIndex),
            pressure = event.getPressure(pointerIndex),
            tilt = event.getAxisValue(MotionEvent.AXIS_TILT, pointerIndex),
            orientation = event.getAxisValue(MotionEvent.AXIS_ORIENTATION, pointerIndex),
            toolType = toolType,
            palmRejected = palmRejected,
            upHasLastMove = action == 2,
            timestampMs = event.eventTime
        )

        if (action == 3) inkOverlay?.onPenCancel()
        if (action == 2 || action == 3) hasLastPoint = false
    }

    private fun sendSample(
        action: Int,
        x: Float,
        y: Float,
        pressure: Float,
        tilt: Float,
        orientation: Float,
        toolType: Int,
        palmRejected: Boolean,
        upHasLastMove: Boolean,
        timestampMs: Long
    ) {
        val insideVideo = videoRect.contains(x, y)
        if (!insideVideo && action != 2 && action != 3) return

        val nx = if (insideVideo) ((x - videoRect.left) / videoRect.width()).coerceIn(0f, 1f) else lastX
        val ny = if (insideVideo) ((y - videoRect.top) / videoRect.height()).coerceIn(0f, 1f) else lastY
        if (insideVideo) {
            lastX = nx
            lastY = ny
            hasLastPoint = true
        }

        // MotionEvent doesn't expose separate tiltX/tiltY axes: AXIS_TILT is
        // the tilt magnitude from vertical and AXIS_ORIENTATION is the
        // compass direction the pen leans toward. Decompose them into
        // (tiltX, tiltY) angles so the PC-side stylus tilt isn't fed the raw
        // orientation value.
        val clampedPressure = pressure.coerceIn(0f, 1f)
        val tiltX = tilt * sin(orientation)
        val tiltY = -tilt * cos(orientation)

        // Draw locally immediately - see InkOverlayView. Skipped for
        // CANCEL (handled by onPenCancel) and for points outside the video
        // area used only to carry the last MOVE coordinate on UP.
        if (insideVideo) {
            when (action) {
                0 -> inkOverlay?.onPenDown(nx, ny, clampedPressure)
                1 -> inkOverlay?.onPenMove(nx, ny, clampedPressure)
                2 -> inkOverlay?.onPenUp()
            }
        }

        penSender?.sendPenData(
            action = action,
            x = nx,
            y = ny,
            pressure = clampedPressure,
            tiltX = tiltX,
            tiltY = tiltY,
            stylus = toolType == MotionEvent.TOOL_TYPE_STYLUS,
            eraser = false,
            palmRejected = palmRejected,
            upHasLastMove = upHasLastMove,
            timestampMs = timestampMs
        )
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        // onMeasure() already constrains this view's bounds to the video's
        // aspect ratio, so the whole view IS the video area - no separate
        // letterbox sub-rect needed here.
        videoRect.set(0f, 0f, w.toFloat(), h.toFloat())
        Log.d(tag, "Size changed: ${w}x$h (video=${videoWidth}x$videoHeight)")
    }

    companion object {
        private const val TOUCH_HEARTBEAT_MS = 100L
        private const val TOUCH_HOLD_MS = 80L   // window to catch the 2nd finger
        private const val TOUCH_SLOP_DP = 8f    // moving this far = it's a drag, send now
        private const val DEFAULT_VIDEO_WIDTH = 1920
        private const val DEFAULT_VIDEO_HEIGHT = 1080
    }
}
