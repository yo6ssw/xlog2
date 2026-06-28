package ro.scripca.xlog2

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Sync
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import kotlinx.coroutines.launch
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.NavHostController

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun LogScreen(nav: NavHostController, vm: LogViewModel = viewModel()) {
    val form by vm.form.collectAsStateWithLifecycle()
    val editingId by vm.editingId.collectAsStateWithLifecycle()
    val dxcc by vm.dxcc.collectAsStateWithLifecycle()
    val dupe by vm.dupe.collectAsStateWithLifecycle()
    val qsos by vm.qsos.collectAsStateWithLifecycle()
    val members by vm.memberCount.collectAsStateWithLifecycle()

    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    var menuOpen by remember { mutableStateOf(false) }

    // SAF pickers for ADIF import/export (bytes ⇄ native Adif.cpp).
    val importLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri -> if (uri != null) scope.launch { AdifIo.import(ctx, uri) } }
    val exportLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("application/octet-stream")
    ) { uri -> if (uri != null) scope.launch { AdifIo.export(ctx, uri) } }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("xlog2") },
                actions = {
                    IconButton(onClick = { nav.navigate(Routes.SYNC) }) {
                        Icon(Icons.Default.Sync, contentDescription = "Sync ($members peers)")
                    }
                    IconButton(onClick = { nav.navigate(Routes.SETTINGS) }) {
                        Icon(Icons.Default.Settings, contentDescription = "Settings")
                    }
                    IconButton(onClick = { menuOpen = true }) {
                        Icon(Icons.Default.MoreVert, contentDescription = "More")
                    }
                    DropdownMenu(expanded = menuOpen, onDismissRequest = { menuOpen = false }) {
                        DropdownMenuItem(
                            text = { Text("Import ADIF…") },
                            onClick = {
                                menuOpen = false
                                importLauncher.launch(arrayOf("*/*"))
                            },
                        )
                        DropdownMenuItem(
                            text = { Text("Export ADIF…") },
                            onClick = {
                                menuOpen = false
                                exportLauncher.launch("xlog2-export.adi")
                            },
                        )
                    }
                },
            )
        },
    ) { pad ->
        Column(Modifier.padding(pad).fillMaxSize().padding(horizontal = 12.dp)) {
            EntryForm(
                form = form,
                editingId = editingId,
                dxcc = dxcc,
                dupe = dupe,
                vm = vm,
            )
            Text(
                "Recent QSOs (${qsos.size})",
                style = MaterialTheme.typography.labelMedium,
                modifier = Modifier.padding(top = 8.dp, bottom = 4.dp),
            )
            LazyColumn(Modifier.fillMaxSize()) {
                items(qsos, key = { it.id }) { q ->
                    QsoRow(q) { vm.loadForEdit(q) }
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun EntryForm(
    form: Qso,
    editingId: Long,
    dxcc: String,
    dupe: String,
    vm: LogViewModel,
) {
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        // Prominent Call field with an inline QRZ-lookup button.
        OutlinedTextField(
            value = form.call,
            onValueChange = { vm.update { q -> q.copy(call = it.uppercase()) }; vm.onKeyFieldsChanged() },
            label = { Text("Call") },
            singleLine = true,
            textStyle = MaterialTheme.typography.headlineSmall.copy(
                fontWeight = FontWeight.Bold, fontFamily = FontFamily.Monospace,
            ),
            trailingIcon = {
                IconButton(onClick = { vm.lookupCall() }) {
                    Icon(Icons.Default.Search, contentDescription = "QRZ lookup")
                }
            },
            keyboardOptions = KeyboardOptions(
                capitalization = KeyboardCapitalization.Characters,
                imeAction = ImeAction.Next,
            ),
            modifier = Modifier.fillMaxWidth(),
        )

        if (dxcc.isNotEmpty()) {
            Text(dxcc, color = MaterialTheme.colorScheme.secondary,
                style = MaterialTheme.typography.bodyMedium)
        }
        if (dupe.isNotEmpty()) {
            Card(Modifier.fillMaxWidth()) {
                Text("⚠ $dupe", color = MaterialTheme.colorScheme.error,
                    modifier = Modifier.padding(8.dp))
            }
        }

        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            Field("Freq", form.freq, Modifier.weight(1f), KeyboardType.Decimal) {
                vm.onFreqChanged(it)
            }
            Field("Band", form.band, Modifier.weight(1f)) {
                vm.update { q -> q.copy(band = it) }; vm.onKeyFieldsChanged()
            }
            Field("Mode", form.mode, Modifier.weight(1f)) {
                vm.update { q -> q.copy(mode = it.uppercase()) }; vm.onKeyFieldsChanged()
            }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            Field("RST s", form.rstSent, Modifier.weight(1f)) { vm.update { q -> q.copy(rstSent = it) } }
            Field("RST r", form.rstRcvd, Modifier.weight(1f)) { vm.update { q -> q.copy(rstRcvd = it) } }
            Field("Power", form.power, Modifier.weight(1f), KeyboardType.Number) {
                vm.update { q -> q.copy(power = it) }
            }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            Field("Name", form.name, Modifier.weight(1f)) { vm.update { q -> q.copy(name = it) } }
            Field("Grid", form.locator, Modifier.weight(1f)) {
                vm.update { q -> q.copy(locator = it.uppercase()) }
            }
        }
        Field("QTH", form.qth, Modifier.fillMaxWidth()) { vm.update { q -> q.copy(qth = it) } }
        Field("Comment", form.comment, Modifier.fillMaxWidth()) { vm.update { q -> q.copy(comment = it) } }

        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
            Button(
                onClick = { vm.save() },
                modifier = Modifier.weight(1f),
            ) {
                Text(if (editingId != 0L) "UPDATE" else "LOG", fontSize = 18.sp)
            }
            if (editingId != 0L) {
                TextButton(onClick = { vm.delete() }) { Text("Delete") }
            }
            TextButton(onClick = { vm.clear() }) { Text("Clear") }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun Field(
    label: String,
    value: String,
    modifier: Modifier = Modifier,
    keyboardType: KeyboardType = KeyboardType.Text,
    onChange: (String) -> Unit,
) {
    OutlinedTextField(
        value = value,
        onValueChange = onChange,
        label = { Text(label) },
        singleLine = true,
        keyboardOptions = KeyboardOptions(keyboardType = keyboardType, imeAction = ImeAction.Next),
        modifier = modifier,
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun QsoRow(q: Qso, onClick: () -> Unit) {
    Card(
        onClick = onClick,
        modifier = Modifier.fillMaxWidth().padding(vertical = 3.dp),
    ) {
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                q.call,
                fontFamily = FontFamily.Monospace,
                fontWeight = FontWeight.Bold,
                modifier = Modifier.weight(1f),
            )
            Column(horizontalAlignment = Alignment.End, modifier = Modifier.weight(1.4f)) {
                Text("${q.band}  ${q.mode}", style = MaterialTheme.typography.bodySmall)
                Text(
                    if (q.country.isNotEmpty()) q.country else q.date,
                    style = MaterialTheme.typography.bodySmall,
                    maxLines = 1, overflow = TextOverflow.Ellipsis,
                )
            }
            Text(
                "${q.date} ${q.timeOn}",
                style = MaterialTheme.typography.bodySmall,
                modifier = Modifier.padding(start = 8.dp),
            )
        }
    }
}
