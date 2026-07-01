package ro.scripca.xlog2

import android.content.Context
import androidx.core.content.edit

/**
 * Persisted station + sync + QRZ settings, mirroring the desktop's [station] /
 * [sync] / [qrz] config blocks. The desktop Settings has no operator callsign,
 * so the phone keeps its own (myCallsign). Stored in SharedPreferences (the
 * Android-idiomatic equivalent of layout.ini); the secret is the mesh PSK.
 */
class Settings(context: Context) {
    private val prefs = context.getSharedPreferences("xlog2", Context.MODE_PRIVATE)

    var myCallsign: String
        get() = prefs.getString("my_callsign", "") ?: ""
        set(v) = prefs.edit { putString("my_callsign", v) }

    var myLocator: String
        get() = prefs.getString("my_locator", "") ?: ""
        set(v) = prefs.edit { putString("my_locator", v) }

    // --- [sync] ---
    var syncEnabled: Boolean
        get() = prefs.getBoolean("sync_enabled", true)
        set(v) = prefs.edit { putBoolean("sync_enabled", v) }

    var syncSecret: String
        get() = prefs.getString("sync_secret", "") ?: ""
        set(v) = prefs.edit { putString("sync_secret", v) }

    var syncNodeName: String
        get() = prefs.getString("sync_node_name", "") ?: ""
        set(v) = prefs.edit { putString("sync_node_name", v) }

    /** Optional WAN peers ("host" or "host:port"), newline-separated in the UI. */
    var staticPeers: List<String>
        get() = (prefs.getString("sync_static_peers", "") ?: "")
            .split('\n').map { it.trim() }.filter { it.isNotEmpty() }
        set(v) = prefs.edit { putString("sync_static_peers", v.joinToString("\n")) }

    var syncPort: Int
        get() = prefs.getInt("sync_port", 7388)
        set(v) = prefs.edit { putInt("sync_port", v) }

    /** Per-node trust allowlist of mesh ids the operator admitted. */
    var trustedIds: List<String>
        get() = (prefs.getString("sync_trusted_ids", "") ?: "")
            .split('\n').map { it.trim() }.filter { it.isNotEmpty() }
        set(v) = prefs.edit { putString("sync_trusted_ids", v.joinToString("\n")) }

    /** Whether to enforce the trust allowlist (identity mesh). */
    var trustEnforce: Boolean
        get() = prefs.getBoolean("sync_trust_enforce", false)
        set(v) = prefs.edit { putBoolean("sync_trust_enforce", v) }

    // --- [qrz] (qrz.com leg handled in Kotlin/OkHttp) ---
    var qrzUser: String
        get() = prefs.getString("qrz_user", "") ?: ""
        set(v) = prefs.edit { putString("qrz_user", v) }

    var qrzPassword: String
        get() = prefs.getString("qrz_password", "") ?: ""
        set(v) = prefs.edit { putString("qrz_password", v) }

    // --- [audio] — cwsd Opus-over-UDP rig-audio stream (see AudioStreamClient) ---
    // Default is an IP, not an mDNS ".local" name: Android's resolver can't do
    // mDNS, so a "*.local" host would fail to connect (the desktop resolves it
    // via nss-mdns). Edit in Settings for a different server.
    var audioHost: String
        get() = prefs.getString("audio_host", "192.168.3.41") ?: "192.168.3.41"
        set(v) = prefs.edit { putString("audio_host", v) }

    var audioPort: Int
        get() = prefs.getInt("audio_port", 7355)
        set(v) = prefs.edit { putInt("audio_port", v) }

    /** Opus sample rate; must match the server (8000/12000/16000/24000/48000). */
    var audioSampleRate: Int
        get() = prefs.getInt("audio_sample_rate", 8000)
        set(v) = prefs.edit { putInt("audio_sample_rate", v) }

    var audioChannels: Int
        get() = prefs.getInt("audio_channels", 1)
        set(v) = prefs.edit { putInt("audio_channels", v) }

    // --- [rig] — Hamlib rigctld frequency polling (see RigctldClient) ---
    var freqEnabled: Boolean
        get() = prefs.getBoolean("freq_enabled", true)
        set(v) = prefs.edit { putBoolean("freq_enabled", v) }

    /** rigctld host; blank falls back to [audioHost] (cwsd usually serves both). */
    var freqHost: String
        get() = prefs.getString("freq_host", "") ?: ""
        set(v) = prefs.edit { putString("freq_host", v) }

    var freqPort: Int
        get() = prefs.getInt("freq_port", 4532)
        set(v) = prefs.edit { putInt("freq_port", v) }

    var freqPollMs: Int
        get() = prefs.getInt("freq_poll_ms", 500)
        set(v) = prefs.edit { putInt("freq_poll_ms", v) }

    /** Effective rigctld host: explicit [freqHost] or, if blank, [audioHost]. */
    val effectiveFreqHost: String get() = freqHost.ifBlank { audioHost }
}
