package ro.scripca.xlog2

import android.content.Context
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

/**
 * The single owner of [XlogCore] for the process. Started once (by the
 * [SyncService]); the UI observes its Flows. All native calls hop to a
 * background dispatcher because they block on the C++ core thread.
 *
 * Singleton because the native engine binds sockets + opens the logbook once;
 * multiple instances would fight over the mesh port and the .xlog file.
 */
class XlogRepository private constructor(appContext: Context) {

    private val ctx = appContext
    private val core = XlogCore()
    private val settings = Settings(appContext)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val dataDir = File(appContext.filesDir, "xlog2").apply { mkdirs() }
    private val qrzClient = QrzClient()
    private val qrzCache = QrzCache(appContext)

    private val _qsos = MutableStateFlow<List<Qso>>(emptyList())
    val qsos: StateFlow<List<Qso>> = _qsos.asStateFlow()

    private val _peers = MutableStateFlow<List<PeerInfo>>(emptyList())
    val peers: StateFlow<List<PeerInfo>> = _peers.asStateFlow()

    private val _memberCount = MutableStateFlow(0)
    val memberCount: StateFlow<Int> = _memberCount.asStateFlow()

    private val _localId = MutableStateFlow("")
    val localId: StateFlow<String> = _localId.asStateFlow()

    private val _identityKey = MutableStateFlow("")
    val identityKey: StateFlow<String> = _identityKey.asStateFlow()

    val status: SharedFlow<String> get() = core.status

    // QRZ resolves in Kotlin (native QRZ is compiled out of the mobile build),
    // so the repository owns the result stream rather than passing core's through.
    private val _qrz = MutableSharedFlow<QrzResult>(extraBufferCapacity = 16)
    val qrz: SharedFlow<QrzResult> = _qrz.asSharedFlow()

    val isRunning: Boolean get() = core.isRunning

    init {
        // Re-query the logbook / peers whenever native signals a change.
        scope.launch { core.logbookRevision.collect { refreshQsos() } }
        scope.launch {
            core.peersRevision.collect {
                _peers.value = core.peerSnapshot()
                _memberCount.value = core.memberCount()
            }
        }
    }

    fun start() {
        if (core.isRunning) return
        copyAssetIfMissing("cty.dat")
        val cty = File(dataDir, "cty.dat")
        core.start(
            XlogCore.Config(
                dataDir = dataDir.absolutePath,
                secret = settings.syncSecret,
                nodeName = settings.syncNodeName.ifEmpty { settings.myCallsign.ifEmpty { "xlog2-android" } },
                ctyPath = if (cty.exists()) cty.absolutePath else "",
                staticPeers = settings.staticPeers,
                syncPort = settings.syncPort,
                trustEnforce = settings.trustEnforce,
                trustedIds = settings.trustedIds,
                requireIdentity = true,
            )
        )
        _localId.value = core.localId()
        _identityKey.value = core.identityKey()
        refreshQsos()
    }

    fun stop() = core.stop()

    /** Apply changed settings (new secret/peers/creds) by bouncing the core. */
    fun restart() {
        core.stop()
        _localId.value = ""
        _identityKey.value = ""
        start()
    }

    private fun refreshQsos() {
        _qsos.value = core.listQsos().sortedWith(
            compareBy({ it.date }, { it.timeOn }, { it.id })
        )
    }

    // --- logbook mutations (off the main thread) --------------------------
    fun addQso(q: Qso) = scope.launch { core.addQso(q) }
    fun updateQso(id: Long, q: Qso) = scope.launch { core.updateQso(id, q) }
    fun deleteQso(id: Long) = scope.launch { core.deleteQso(id) }

    // --- live form helpers (suspending; native blocks) --------------------
    suspend fun deriveDxcc(call: String): Dxcc =
        withContext(Dispatchers.IO) { core.deriveDxcc(call) }

    suspend fun bandForFreq(mhz: Double): String =
        withContext(Dispatchers.IO) { core.bandForFreq(mhz) }

    suspend fun findDupe(q: Qso, editId: Long): String =
        withContext(Dispatchers.IO) { core.findDupe(q, editId) }

    // --- ADIF -------------------------------------------------------------
    suspend fun importAdif(text: String): Int =
        withContext(Dispatchers.IO) { core.importAdif(text) }

    suspend fun exportAdif(): String =
        withContext(Dispatchers.IO) { core.exportAdif() }

    // --- QRZ --------------------------------------------------------------
    // Two-tier resolve, mirroring the desktop tiers available on mobile:
    // on-device cache → qrz.com (OkHttp). The native QRZ subsystem (cache, mesh
    // peers, libcurl network) is compiled out of the mobile build, so this is
    // the whole path; results are published on [qrz] for the UI to consume.
    fun lookup(call: String) = scope.launch {
        val c = call.trim().uppercase()
        if (c.isEmpty()) return@launch

        qrzCache.get(c)?.let { _qrz.emit(it); return@launch }

        val user = settings.qrzUser
        if (user.isEmpty()) {
            _qrz.emit(QrzResult(c, error = "Set your QRZ username/password in Settings"))
            return@launch
        }

        val r = qrzClient.lookup(c, user, settings.qrzPassword)
        if (r.error.isEmpty()) qrzCache.put(r)
        _qrz.emit(r)
    }

    // --- sync trust -------------------------------------------------------
    fun trustPeer(id: String) = scope.launch {
        core.trustPeer(id)
        settings.trustedIds = (settings.trustedIds + id).distinct()
    }
    fun revokePeer(id: String) = scope.launch {
        core.revokePeer(id)
        settings.trustedIds = settings.trustedIds - id
    }
    fun syncNow() = scope.launch { core.syncNow() }

    private fun copyAssetIfMissing(name: String) {
        val dst = File(dataDir, name)
        if (dst.exists() && dst.length() > 0) return
        runCatching {
            ctx.assets.open(name).use { input ->
                dst.outputStream().use { input.copyTo(it) }
            }
        }
    }

    companion object {
        @Volatile private var instance: XlogRepository? = null

        fun get(context: Context): XlogRepository =
            instance ?: synchronized(this) {
                instance ?: XlogRepository(context.applicationContext).also { instance = it }
            }
    }
}
