// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

package ro.scripca.xlog2

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone

/**
 * Drives the Log screen: holds the entry-form state and recomputes the live
 * indicators (DXCC, dupe, band-from-freq) the same way LogPagePresenter does on
 * the desktop — only here the recompute calls into the native core. The recent
 * QSO list comes straight from the repository's logbook Flow.
 */
class LogViewModel(app: Application) : AndroidViewModel(app) {

    private val repo = XlogRepository.get(app)

    val qsos: StateFlow<List<Qso>> = repo.qsos
    val memberCount: StateFlow<Int> = repo.memberCount

    private val _form = MutableStateFlow(freshForm())
    val form: StateFlow<Qso> = _form.asStateFlow()

    /** 0 = logging a new QSO; non-zero = editing that stored row. */
    private val _editingId = MutableStateFlow(0L)
    val editingId: StateFlow<Long> = _editingId.asStateFlow()

    private val _dxcc = MutableStateFlow("")
    val dxcc: StateFlow<String> = _dxcc.asStateFlow()

    private val _dupe = MutableStateFlow("")
    val dupe: StateFlow<String> = _dupe.asStateFlow()

    /** A QRZ lookup is in flight (drives the spinner on the Call field). */
    private val _qrzBusy = MutableStateFlow(false)
    val qrzBusy: StateFlow<Boolean> = _qrzBusy.asStateFlow()

    /** Transient QRZ feedback ("No record", an error, …); "" when idle/cleared. */
    private val _qrzStatus = MutableStateFlow("")
    val qrzStatus: StateFlow<String> = _qrzStatus.asStateFlow()

    private var indicatorJob: Job? = null

    init {
        viewModelScope.launch {
            repo.qrz.collect { r ->
                _qrzBusy.value = false
                // Ignore stale results for a call we're no longer entering.
                if (!r.call.equals(_form.value.call, ignoreCase = true)) return@collect
                if (r.error.isEmpty()) {
                    // Auto-fill blank name/QTH/grid from the hit.
                    _form.value = _form.value.copy(
                        name = _form.value.name.ifEmpty { r.name },
                        qth = _form.value.qth.ifEmpty { r.qth },
                        locator = _form.value.locator.ifEmpty { r.locator },
                    )
                    _qrzStatus.value = ""
                } else {
                    _qrzStatus.value = r.error
                }
            }
        }
    }

    fun update(transform: (Qso) -> Qso) {
        _form.value = transform(_form.value)
    }

    /** Call/band/mode/date/freq changed → refresh DXCC + dupe (debounced). */
    fun onKeyFieldsChanged() {
        indicatorJob?.cancel()
        indicatorJob = viewModelScope.launch {
            delay(150)  // debounce typing
            val f = _form.value
            _dxcc.value = repo.deriveDxcc(f.call).format()
            _dupe.value = if (f.call.isBlank()) "" else repo.findDupe(f, _editingId.value)
        }
    }

    fun onFreqChanged(freq: String) {
        _form.value = _form.value.copy(freq = freq)
        viewModelScope.launch {
            val mhz = freq.toDoubleOrNull() ?: return@launch
            val band = repo.bandForFreq(mhz)
            if (band.isNotEmpty()) _form.value = _form.value.copy(band = band)
        }
    }

    fun lookupCall() {
        val call = _form.value.call.trim()
        if (call.isEmpty()) return
        _qrzStatus.value = ""
        _qrzBusy.value = true
        repo.lookup(call)
    }

    /** Begin a new QSO. Date/time = now, RST 599, and freq/band/mode carried
     *  over from the most recent logged QSO (the repo list is ascending, so the
     *  newest is last). Blank when the logbook is empty. When rigctld is
     *  connected the rig's live frequency wins over the carried-over value and
     *  its band is auto-derived — matching the desktop, which tracks the rig
     *  into the entry form. */
    fun startAdd() {
        val last = repo.qsos.value.lastOrNull()
        _editingId.value = 0L
        _form.value = freshForm().copy(
            freq = last?.freq ?: "",
            band = last?.band ?: "",
            mode = last?.mode ?: "",
        )
        _dxcc.value = ""
        _dupe.value = ""
        _qrzStatus.value = ""
        _qrzBusy.value = false

        val rigHz = if (repo.rigConnected.value) repo.rigFreqHz.value else null
        if (rigHz != null && rigHz > 0) onFreqChanged(formatMhz(rigHz))
    }

    /** Load a stored QSO into the form for editing. */
    fun startEdit(q: Qso) {
        _editingId.value = q.id
        _form.value = q
        _qrzStatus.value = ""
        _qrzBusy.value = false
        onKeyFieldsChanged()
    }

    /** Persist the form. Returns false (no-op) when there's no callsign, so the
     *  entry screen can stay open; true once added/updated. */
    fun save(): Boolean {
        val f = _form.value
        if (f.call.isBlank()) return false
        if (_editingId.value != 0L) repo.updateQso(_editingId.value, f)
        else repo.addQso(f)
        cancel()
        return true
    }

    /** Delete a specific QSO from the list (propagates a tombstone). */
    fun delete(q: Qso) {
        if (q.id != 0L) repo.deleteQso(q.id)
    }

    /** Discard the in-progress add/edit and reset the form. */
    fun cancel() {
        _editingId.value = 0L
        _form.value = freshForm()
        _dxcc.value = ""
        _dupe.value = ""
    }

    private fun freshForm(): Qso = Qso(
        date = utc("yyyy-MM-dd"),
        timeOn = utc("HH:mm"),
        rstSent = "599",
        rstRcvd = "599",
    )

    private fun utc(pattern: String): String =
        SimpleDateFormat(pattern, Locale.US).apply {
            timeZone = TimeZone.getTimeZone("UTC")
        }.format(Date())

    /** 14074000 Hz -> "14.074" MHz (trailing zeros trimmed), as the form/desktop
     *  store frequencies. */
    private fun formatMhz(hz: Long): String {
        var s = "%.6f".format(Locale.US, hz / 1_000_000.0)
        if ('.' in s) s = s.trimEnd('0').trimEnd('.')
        return s
    }
}
