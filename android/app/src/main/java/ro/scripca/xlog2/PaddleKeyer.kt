package ro.scripca.xlog2

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Kotlin front for the C++ RemotePaddleKeyer (JNI in paddle_jni.cpp). The
 * jitter-critical iambic keyer, UDP edge streaming and AAudio sidetone run
 * natively; this feeds paddle contacts ([setDit]/[setDah], driven by the USB HID
 * reader or on-screen buttons) and surfaces status + transmit state as flows.
 */
class PaddleKeyer {

    data class Config(
        val host: String, val port: Int, val wpm: Int,
        val iambicB: Boolean, val ultimatic: Boolean, val autospace: Boolean,
        val sidetone: Boolean, val toneHz: Int, val level: Int, val muteTailMs: Int,
    )

    private var handle: Long = 0L

    private val _status = MutableStateFlow("")
    val status: StateFlow<String> = _status.asStateFlow()

    /** Transmit on/off, with the keyer's mute hang. */
    private val _transmitting = MutableStateFlow(false)
    val transmitting: StateFlow<Boolean> = _transmitting.asStateFlow()

    private val _active = MutableStateFlow(false)
    val active: StateFlow<Boolean> = _active.asStateFlow()

    /** Wired by the repository to mute the rig-audio stream while transmitting. */
    var onTransmitChange: ((Boolean) -> Unit)? = null

    fun start(cfg: Config) {
        if (handle == 0L) handle = nativeCreate(this)
        nativeStart(
            handle, cfg.host, cfg.port, cfg.wpm, cfg.iambicB, cfg.ultimatic,
            cfg.autospace, cfg.sidetone, cfg.toneHz, cfg.level, cfg.muteTailMs,
        )
        _active.value = true
    }

    fun stop() {
        if (handle != 0L) nativeStop(handle)
        _active.value = false
        _transmitting.value = false
    }

    fun setDit(pressed: Boolean) { if (handle != 0L) nativeSetDit(handle, pressed) }
    fun setDah(pressed: Boolean) { if (handle != 0L) nativeSetDah(handle, pressed) }

    /** Free the native keyer entirely (process teardown). */
    fun release() {
        if (handle != 0L) { nativeDestroy(handle); handle = 0L }
        _active.value = false
        _transmitting.value = false
    }

    // --- callbacks from native (keyer worker thread; flows are thread-safe) ---
    @Suppress("unused") private fun onStatus(msg: String) { _status.value = msg }
    @Suppress("unused") private fun onTransmit(tx: Boolean) {
        _transmitting.value = tx
        onTransmitChange?.invoke(tx)
    }

    private external fun nativeCreate(listener: PaddleKeyer): Long
    private external fun nativeStart(
        h: Long, host: String, port: Int, wpm: Int, iambicB: Boolean, ultimatic: Boolean,
        autospace: Boolean, sidetone: Boolean, toneHz: Int, level: Int, muteTailMs: Int,
    )
    private external fun nativeStop(h: Long)
    private external fun nativeSetDit(h: Long, pressed: Boolean)
    private external fun nativeSetDah(h: Long, pressed: Boolean)
    private external fun nativeDestroy(h: Long)

    companion object {
        init { System.loadLibrary("xlog2jni") }
    }
}
