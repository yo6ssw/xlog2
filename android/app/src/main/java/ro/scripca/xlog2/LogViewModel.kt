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

    private var indicatorJob: Job? = null

    init {
        viewModelScope.launch {
            repo.qrz.collect { r ->
                // Auto-fill blank name/QTH/grid from a QRZ hit for the current call.
                if (r.error.isEmpty() && r.call.equals(_form.value.call, ignoreCase = true)) {
                    _form.value = _form.value.copy(
                        name = _form.value.name.ifEmpty { r.name },
                        qth = _form.value.qth.ifEmpty { r.qth },
                        locator = _form.value.locator.ifEmpty { r.locator },
                    )
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
        if (call.isNotEmpty()) repo.lookup(call)
    }

    fun save() {
        val f = _form.value
        if (f.call.isBlank()) return
        if (_editingId.value != 0L) repo.updateQso(_editingId.value, f)
        else repo.addQso(f)
        clear()
    }

    fun loadForEdit(q: Qso) {
        _editingId.value = q.id
        _form.value = q
        onKeyFieldsChanged()
    }

    fun delete() {
        if (_editingId.value != 0L) repo.deleteQso(_editingId.value)
        clear()
    }

    fun clear() {
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
}
