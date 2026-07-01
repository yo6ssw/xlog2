package ro.scripca.xlog2

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetSocketAddress
import java.net.SocketTimeoutException

/**
 * Subscribes to a cwsd `audio_stream_server` Opus-over-UDP rig-audio stream and
 * plays it back through an [AudioTrack]. The Android counterpart of the desktop
 * [src/core/services/Audio.cpp] — but where the desktop reuses the C++ core's
 * PipeWire backend, here (as with QRZ → OkHttp) the network + playback are pure
 * Kotlin and only the Opus decode crosses JNI ([OpusDecoder]).
 *
 * cwsd has no configured target: a client subscribes by sending any datagram and
 * stays subscribed by continuing to send (a ~2 s keepalive); silent clients are
 * dropped. Wire format (server → client): a 4-byte big-endian sequence number
 * followed by a raw Opus packet. AudioTrack's own buffer absorbs jitter, so the
 * elaborate drift compensation of the desktop backend isn't needed; lost packets
 * are concealed with Opus PLC to keep element timing intact.
 */
class AudioStreamClient {

    data class Config(val host: String, val port: Int, val sampleRate: Int, val channels: Int)

    private val _streaming = MutableStateFlow(false)
    val streaming: StateFlow<Boolean> = _streaming.asStateFlow()

    private val _status = MutableStateFlow("")
    val status: StateFlow<String> = _status.asStateFlow()

    /** Running decoded-frame count, republished ~once a second while streaming. */
    private val _framesDecoded = MutableStateFlow(0L)
    val framesDecoded: StateFlow<Long> = _framesDecoded.asStateFlow()

    @Volatile private var running = false
    @Volatile private var muted = false
    private var worker: Thread? = null

    /**
     * Mute playback without unsubscribing: keep receiving + decoding (so the
     * stream stays subscribed and the decoder stays in sync) but play silence.
     * Used for semi-break-in — the paddle keyer silences the rig audio while
     * transmitting so you don't hear your own delayed signal fighting the sidetone.
     */
    fun setMuted(m: Boolean) { muted = m }

    fun start(cfg: Config) {
        stop()
        if (cfg.host.isBlank() || cfg.port !in 1..65535) {
            _status.value = "Audio: invalid host/port"
            return
        }
        running = true
        worker = Thread({ run(cfg) }, "xlog2-audio").apply { isDaemon = true; start() }
    }

    fun stop() {
        val wasRunning = running
        running = false
        worker?.let { it.interrupt(); it.join(1500) }
        worker = null
        if (wasRunning) _status.value = "Audio: stopped"
    }

    val isStreaming: Boolean get() = _streaming.value

    private fun run(cfg: Config) {
        val dec = OpusDecoder(cfg.sampleRate, cfg.channels)
        if (!dec.ok) {
            _status.value = "Audio: Opus decoder init failed"
            running = false
            return
        }
        var socketRef: DatagramSocket? = null
        var trackRef: AudioTrack? = null
        try {
            _status.value = "Audio: connecting to ${cfg.host}:${cfg.port}…"
            val socket = DatagramSocket().apply {
                connect(InetSocketAddress(cfg.host, cfg.port))  // pin the peer
                soTimeout = 250                                 // so we can poll `running`
            }
            socketRef = socket
            val track = buildTrack(cfg).apply { play() }
            trackRef = track

            val maxFrame = 5760                                  // 120 ms @ 48 kHz
            val pcm = ShortArray(maxFrame * cfg.channels)
            val rx = ByteArray(1500)
            val packet = DatagramPacket(rx, rx.size)
            var haveSeq = false
            var expected = 0L
            var lastFrameSamples = (cfg.sampleRate / 50).coerceAtLeast(1)  // 20 ms fallback
            var frames = 0L
            var lastKeepalive = 0L
            var lastStats = System.nanoTime()
            val ka = ByteArray(1)

            _status.value = "Audio: streaming from ${cfg.host}:${cfg.port}"
            _streaming.value = true

            while (running) {
                val now = System.nanoTime()
                if (now - lastKeepalive >= 2_000_000_000L) {
                    runCatching { socket.send(DatagramPacket(ka, 1)) }  // any datagram (re)subscribes
                    lastKeepalive = now
                }
                if (now - lastStats >= 1_000_000_000L) {
                    _framesDecoded.value = frames
                    lastStats = now
                }

                try {
                    packet.length = rx.size
                    socket.receive(packet)
                } catch (e: SocketTimeoutException) {
                    continue
                }

                val n = packet.length
                if (n <= HEADER) continue
                val seq = ((rx[0].toLong() and 0xFF) shl 24) or ((rx[1].toLong() and 0xFF) shl 16) or
                          ((rx[2].toLong() and 0xFF) shl 8) or (rx[3].toLong() and 0xFF)

                // Conceal any gap between the last packet and this one with PLC.
                if (haveSeq) {
                    val gap = (seq - expected) and 0xFFFFFFFFL
                    if (gap in 1..MAX_CONCEAL) {
                        repeat(gap.toInt()) {
                            val g = dec.decodePlc(pcm, lastFrameSamples)
                            if (g > 0) {
                                if (muted) pcm.fill(0.toShort(), 0, g * cfg.channels)
                                track.write(pcm, 0, g * cfg.channels); frames++
                            }
                        }
                    }
                    // gap == 0: in order; huge gap: reorder/restart — just decode this one.
                }

                val opus = rx.copyOfRange(HEADER, n)
                val got = dec.decode(opus, opus.size, pcm, maxFrame)
                if (got > 0) {
                    if (muted) pcm.fill(0.toShort(), 0, got * cfg.channels)
                    track.write(pcm, 0, got * cfg.channels)
                    lastFrameSamples = got
                    frames++
                    expected = (seq + 1) and 0xFFFFFFFFL
                    haveSeq = true
                }
            }
        } catch (e: Exception) {
            // Keep the error visible (stop() sets the clean "stopped" message).
            if (running) _status.value = "Audio: ${e.message ?: e.javaClass.simpleName}"
        } finally {
            runCatching { trackRef?.stop() }
            trackRef?.release()
            socketRef?.close()
            dec.close()
            _streaming.value = false
            running = false
        }
    }

    private fun buildTrack(cfg: Config): AudioTrack {
        val channelMask =
            if (cfg.channels >= 2) AudioFormat.CHANNEL_OUT_STEREO else AudioFormat.CHANNEL_OUT_MONO
        val minBuf = AudioTrack.getMinBufferSize(cfg.sampleRate, channelMask, AudioFormat.ENCODING_PCM_16BIT)
        // ~0.5 s cushion so network jitter never underruns the device.
        val cushion = cfg.sampleRate * cfg.channels * 2 / 2
        val bufSize = maxOf(minBuf, cushion)
        return AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build()
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setSampleRate(cfg.sampleRate)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setChannelMask(channelMask)
                    .build()
            )
            .setBufferSizeInBytes(bufSize)
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
    }

    companion object {
        private const val HEADER = 4          // big-endian sequence number
        private const val MAX_CONCEAL = 10    // cap PLC frames per gap; larger = restart
    }
}
