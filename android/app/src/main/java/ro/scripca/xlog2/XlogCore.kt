// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

package ro.scripca.xlog2

import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * The Kotlin half of the JNI bridge to the carved-out C++ sync core
 * (libxlog2jni / xlog_mobile). This is the single contract between the Compose
 * UI and the same engine xlog2 / xlog2-syncd run: log a QSO, sync it over the
 * mesh, resolve DXCC/dupes/QRZ, import/export ADIF, manage trusted peers.
 *
 * QSO rows cross JNI as a single string: fields joined by US (0x1f), rows by RS
 * (0x1e), in the fixed order in [Qso.fromWire]/[Qso.toWire] — kept byte-identical
 * to qsoToWire()/wireToForm() in xlog_jni.cpp.
 *
 * Native callbacks (onLogbookChanged / onPeersChanged / onStatus / onQrzResult)
 * arrive on the C++ core thread; this class republishes them as Flows the UI can
 * collect on the main dispatcher.
 */
class XlogCore {

    companion object {
        const val US = '\u001F'   // unit separator (between fields)
        const val RS = '\u001E'   // record separator (between rows)

        init { System.loadLibrary("xlog2jni") }
    }

    private var handle: Long = 0L

    /** Emits each time the logbook changed (local edit or a remote merge). */
    private val _logbookRevision = MutableStateFlow(0L)
    val logbookRevision: StateFlow<Long> = _logbookRevision

    /** Emits each time the peer set / trust / readiness changed. */
    private val _peersRevision = MutableStateFlow(0L)
    val peersRevision: StateFlow<Long> = _peersRevision

    /** Status-line messages from the sync engine. */
    private val _status = MutableSharedFlow<String>(extraBufferCapacity = 32)
    val status: SharedFlow<String> = _status

    /** Resolved QRZ lookups (mesh peer or qrz.com). */
    private val _qrz = MutableSharedFlow<QrzResult>(extraBufferCapacity = 16)
    val qrz: SharedFlow<QrzResult> = _qrz

    data class Config(
        val dataDir: String,
        val secret: String,
        val nodeName: String,
        val ctyPath: String = "",
        val qrzUser: String = "",
        val qrzPassword: String = "",
        val qrzCacheDays: Int = 365,
        val staticPeers: List<String> = emptyList(),  // "host" or "host:port"
        val syncPort: Int = 7388,
        val trustEnforce: Boolean = false,
        val trustedIds: List<String> = emptyList(),
        val requireIdentity: Boolean = true,
    )

    val isRunning: Boolean get() = handle != 0L

    fun start(cfg: Config) {
        if (handle != 0L) return
        handle = nativeStart(
            this, cfg.dataDir, cfg.secret, cfg.nodeName, cfg.ctyPath,
            cfg.qrzUser, cfg.qrzPassword, cfg.qrzCacheDays,
            cfg.staticPeers.joinToString(RS.toString()), cfg.syncPort,
            cfg.trustEnforce, cfg.trustedIds.joinToString(RS.toString()),
            cfg.requireIdentity,
        )
    }

    fun stop() {
        if (handle == 0L) return
        nativeStop(handle); handle = 0L
    }

    // Every public call is a no-op (empty/0) until the engine is started. The
    // native handle is 0 before start()/after stop(); calling into the bridge
    // then would dereference a null MobileCore*. Guard at this single boundary.
    // --- logbook ----------------------------------------------------------
    fun listQsos(): List<Qso> {
        if (handle == 0L) return emptyList()
        val s = nativeListQsos(handle)
        if (s.isEmpty()) return emptyList()
        return s.split(RS).map { Qso.fromWire(it) }
    }

    /** Add a new QSO; returns the stored row id. */
    fun addQso(q: Qso): Long = if (handle == 0L) 0 else nativeAddQso(handle, q.toWire())

    fun updateQso(id: Long, q: Qso) { if (handle != 0L) nativeUpdateQso(handle, id, q.toWire()) }
    fun deleteQso(id: Long) { if (handle != 0L) nativeDeleteQso(handle, id) }

    // --- live form helpers ------------------------------------------------
    fun deriveDxcc(call: String): Dxcc {
        if (handle == 0L) return Dxcc("", "", "", "")
        val f = nativeDeriveDxcc(handle, call).split(US)
        return Dxcc(
            country = f.getOrElse(0) { "" }, cqZone = f.getOrElse(1) { "" },
            ituZone = f.getOrElse(2) { "" }, continent = f.getOrElse(3) { "" },
        )
    }

    fun bandForFreq(mhz: Double): String =
        if (handle == 0L) "" else nativeBandForFreq(handle, mhz)

    /** The dupe-warning message for [q], or empty if not a dupe. */
    fun findDupe(q: Qso, editId: Long = 0L): String =
        if (handle == 0L) "" else nativeFindDupe(handle, q.toWire(), editId)

    // --- ADIF -------------------------------------------------------------
    fun importAdif(text: String): Int = if (handle == 0L) 0 else nativeImportAdif(handle, text)
    fun exportAdif(): String = if (handle == 0L) "" else nativeExportAdif(handle)

    // --- QRZ --------------------------------------------------------------
    fun lookup(call: String) { if (handle != 0L) nativeLookup(handle, call) }

    // --- sync introspection + trust --------------------------------------
    fun localId(): String = if (handle == 0L) "" else nativeLocalId(handle)
    fun identityKey(): String = if (handle == 0L) "" else nativeIdentityKey(handle)
    fun memberCount(): Int = if (handle == 0L) 0 else nativeMemberCount(handle)

    fun peerSnapshot(): List<PeerInfo> {
        if (handle == 0L) return emptyList()
        val s = nativePeerSnapshot(handle)
        if (s.isEmpty()) return emptyList()
        return s.split(RS).map {
            val f = it.split(US)
            PeerInfo(
                id = f.getOrElse(0) { "" }, name = f.getOrElse(1) { "" },
                trusted = f.getOrElse(2) { "0" } == "1",
                online = f.getOrElse(3) { "0" } == "1",
                ready = f.getOrElse(4) { "0" } == "1",
            )
        }
    }

    fun trustPeer(id: String) { if (handle != 0L) nativeTrustPeer(handle, id) }
    fun revokePeer(id: String) { if (handle != 0L) nativeRevokePeer(handle, id) }
    fun syncNow() { if (handle != 0L) nativeSyncNow(handle) }

    // --- callbacks from native (core thread) — keep them tiny ------------
    @Suppress("unused") private fun onLogbookChanged() { _logbookRevision.value++ }
    @Suppress("unused") private fun onPeersChanged() { _peersRevision.value++ }
    @Suppress("unused") private fun onStatus(msg: String) { _status.tryEmit(msg) }
    @Suppress("unused") private fun onQrzResult(wire: String) {
        val f = wire.split(US)
        _qrz.tryEmit(
            QrzResult(
                call = f.getOrElse(0) { "" }, name = f.getOrElse(1) { "" },
                qth = f.getOrElse(2) { "" }, locator = f.getOrElse(3) { "" },
                country = f.getOrElse(4) { "" }, error = f.getOrElse(5) { "" },
            )
        )
    }

    // --- native methods (must match xlog_jni.cpp exactly) ----------------
    private external fun nativeStart(
        listener: XlogCore, dataDir: String, secret: String, nodeName: String,
        ctyPath: String, qrzUser: String, qrzPass: String, qrzCacheDays: Int,
        staticPeers: String, syncPort: Int, trustEnforce: Boolean,
        trustedIds: String, requireIdentity: Boolean,
    ): Long
    private external fun nativeStop(handle: Long)
    private external fun nativeListQsos(handle: Long): String
    private external fun nativeAddQso(handle: Long, wire: String): Long
    private external fun nativeUpdateQso(handle: Long, id: Long, wire: String)
    private external fun nativeDeleteQso(handle: Long, id: Long)
    private external fun nativeDeriveDxcc(handle: Long, call: String): String
    private external fun nativeBandForFreq(handle: Long, mhz: Double): String
    private external fun nativeFindDupe(handle: Long, wire: String, editId: Long): String
    private external fun nativeImportAdif(handle: Long, text: String): Int
    private external fun nativeExportAdif(handle: Long): String
    private external fun nativeLookup(handle: Long, call: String)
    private external fun nativeLocalId(handle: Long): String
    private external fun nativeIdentityKey(handle: Long): String
    private external fun nativeMemberCount(handle: Long): Int
    private external fun nativePeerSnapshot(handle: Long): String
    private external fun nativeTrustPeer(handle: Long, id: String)
    private external fun nativeRevokePeer(handle: Long, id: String)
    private external fun nativeSyncNow(handle: Long)
}

data class Dxcc(val country: String, val cqZone: String, val ituZone: String, val continent: String) {
    val isEmpty get() = country.isEmpty()
    fun format(): String =
        if (isEmpty) "" else buildString {
            append(country)
            if (cqZone.isNotEmpty()) append("  ·  CQ ").append(cqZone)
            if (ituZone.isNotEmpty()) append("  ·  ITU ").append(ituZone)
            if (continent.isNotEmpty()) append("  ·  ").append(continent)
        }
}

data class PeerInfo(
    val id: String, val name: String,
    val trusted: Boolean, val online: Boolean, val ready: Boolean,
)

data class QrzResult(
    val call: String, val name: String = "", val qth: String = "",
    val locator: String = "", val country: String = "", val error: String = "",
)
