package ro.scripca.xlog2

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.net.InetSocketAddress
import java.net.Socket

/**
 * Polls a Hamlib `rigctld` TCP server for the current rig frequency and lets the
 * operator retune it. This is the same server the desktop reaches through
 * Hamlib's netrigctl backend (model 2): cwsd exposes it alongside the audio
 * stream. The plain (non-extended) protocol answers the one-char `f` command
 * with the frequency in Hz on a single line, e.g. `14074000.000000`, and takes
 * `F <hz>` to set it (replying with a one-line `RPRT` status).
 *
 * Set/step requests are queued and applied by the worker at the top of its next
 * poll tick, so the socket stays worker-thread-only (mirroring the desktop
 * [RigController], whose Hamlib handle is likewise touched on one thread). Rapid
 * step taps accumulate between ticks, so none are lost.
 *
 * Reconnects with a short backoff if the server drops, so the frequency readout
 * recovers on its own.
 */
class RigctldClient {

    /** Current rig frequency in Hz, or null when unknown / disconnected. */
    private val _freqHz = MutableStateFlow<Long?>(null)
    val freqHz: StateFlow<Long?> = _freqHz.asStateFlow()

    private val _connected = MutableStateFlow(false)
    val connected: StateFlow<Boolean> = _connected.asStateFlow()

    @Volatile private var running = false
    private var worker: Thread? = null

    // Queued retune, applied by the worker on its next poll tick. Guarded by
    // [pendingLock]. An absolute set clears any accumulated step; a step adds to
    // the accumulator so quick repeated taps between ticks all land.
    private val pendingLock = Any()
    private var pendingSetHz: Long? = null
    private var pendingStepHz: Long = 0

    fun start(host: String, port: Int, pollMs: Long) {
        stop()
        if (host.isBlank() || port !in 1..65535) return
        running = true
        synchronized(pendingLock) { pendingSetHz = null; pendingStepHz = 0 }
        worker = Thread({ run(host, port, pollMs) }, "xlog2-rigctld").apply { isDaemon = true; start() }
    }

    fun stop() {
        running = false
        worker?.let { it.interrupt(); it.join(1500) }
        worker = null
        _connected.value = false
        _freqHz.value = null
        synchronized(pendingLock) { pendingSetHz = null; pendingStepHz = 0 }
    }

    /** Queue an absolute VFO frequency (Hz); applied on the worker's next tick. */
    fun setFrequency(hz: Long) {
        if (hz <= 0) return
        synchronized(pendingLock) { pendingSetHz = hz; pendingStepHz = 0 }
    }

    /** Queue a relative VFO nudge (signed Hz); accumulates between ticks. */
    fun stepFrequency(deltaHz: Long) {
        synchronized(pendingLock) { pendingStepHz += deltaHz }
    }

    private fun run(host: String, port: Int, pollMs: Long) {
        while (running) {
            try {
                Socket().use { s ->
                    s.connect(InetSocketAddress(host, port), 3000)
                    s.soTimeout = 3000
                    _connected.value = true
                    val out = s.getOutputStream()
                    val reader = s.getInputStream().bufferedReader()
                    while (running) {
                        // Apply any queued retune first, so the readout below
                        // reflects it on the same tick.
                        val target = synchronized(pendingLock) {
                            val set = pendingSetHz
                            val step = pendingStepHz
                            pendingSetHz = null
                            pendingStepHz = 0
                            when {
                                set != null -> set + step
                                step != 0L  -> _freqHz.value?.let { it + step }
                                else        -> null
                            }
                        }
                        if (target != null && target > 0) {
                            out.write("F $target\n".toByteArray())
                            out.flush()
                            reader.readLine() ?: return@use   // consume RPRT reply
                            _freqHz.value = target             // optimistic; confirmed below
                        }

                        out.write(FREQ_CMD)
                        out.flush()
                        val line = reader.readLine() ?: break   // server closed
                        // Non-extended rigctld: just the value. Ignore RPRT/errors.
                        line.trim().toDoubleOrNull()?.toLong()?.let { hz ->
                            if (hz > 0) _freqHz.value = hz
                        }
                        Thread.sleep(pollMs)
                    }
                }
            } catch (e: InterruptedException) {
                break
            } catch (e: Exception) {
                _connected.value = false
                _freqHz.value = null
                if (!running) break
                try { Thread.sleep(2000) } catch (e: InterruptedException) { break }  // retry backoff
            }
        }
        _connected.value = false
        _freqHz.value = null
    }

    companion object {
        private val FREQ_CMD = "f\n".toByteArray()
    }
}
