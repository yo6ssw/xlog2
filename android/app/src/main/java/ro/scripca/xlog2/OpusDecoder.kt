package ro.scripca.xlog2

/**
 * Thin Kotlin front for the vendored libopus decoder (JNI in opus_jni.cpp). One
 * instance owns one native OpusDecoder handle; not thread-safe — use it from a
 * single decode thread (AudioStreamClient's worker).
 */
class OpusDecoder(sampleRate: Int, channels: Int) {

    private var handle: Long = nativeCreate(sampleRate, channels)

    /** True if the native decoder was created successfully. */
    val ok: Boolean get() = handle != 0L

    /**
     * Decode one Opus packet into [pcm] (interleaved int16). [frameSize] is the
     * per-channel capacity of [pcm]. Returns samples decoded per channel, or a
     * negative Opus error code.
     */
    fun decode(packet: ByteArray, len: Int, pcm: ShortArray, frameSize: Int): Int =
        if (handle == 0L) -1 else nativeDecode(handle, packet, len, pcm, frameSize)

    /** Packet-loss concealment: synthesise one [frameSize]-sample frame. */
    fun decodePlc(pcm: ShortArray, frameSize: Int): Int =
        if (handle == 0L) -1 else nativeDecodePlc(handle, pcm, frameSize)

    fun close() {
        if (handle != 0L) { nativeDestroy(handle); handle = 0L }
    }

    private external fun nativeCreate(rate: Int, channels: Int): Long
    private external fun nativeDecode(h: Long, packet: ByteArray, len: Int, pcm: ShortArray, frameSize: Int): Int
    private external fun nativeDecodePlc(h: Long, pcm: ShortArray, frameSize: Int): Int
    private external fun nativeDestroy(h: Long)

    companion object {
        init { System.loadLibrary("xlog2jni") }
    }
}
