package ro.scripca.xlog2

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.net.InetSocketAddress
import java.net.Socket

/**
 * Polls a Hamlib `rigctld` TCP server for the current rig frequency. This is the
 * same server the desktop reaches through Hamlib's netrigctl backend (model 2):
 * cwsd exposes it alongside the audio stream. The plain (non-extended) protocol
 * answers the one-char `f` command with the frequency in Hz on a single line,
 * e.g. `14074000.000000`.
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

    fun start(host: String, port: Int, pollMs: Long) {
        stop()
        if (host.isBlank() || port !in 1..65535) return
        running = true
        worker = Thread({ run(host, port, pollMs) }, "xlog2-rigctld").apply { isDaemon = true; start() }
    }

    fun stop() {
        running = false
        worker?.let { it.interrupt(); it.join(1500) }
        worker = null
        _connected.value = false
        _freqHz.value = null
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
