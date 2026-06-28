package ro.scripca.xlog2

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.outlined.Public
import androidx.compose.material.icons.outlined.Warning
import androidx.compose.material3.Button
import androidx.compose.material3.ElevatedCard
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.navigation.NavHostController

/**
 * The add/edit screen: the QSO entry form with live DXCC/dupe indicators and a
 * QRZ lookup, plus Save and Cancel. Reached from the list (FAB = new,
 * tap = edit); Cancel / system-back returns to the list without saving.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun EntryScreen(nav: NavHostController, vm: LogViewModel) {
    val form by vm.form.collectAsStateWithLifecycle()
    val editingId by vm.editingId.collectAsStateWithLifecycle()
    val dxcc by vm.dxcc.collectAsStateWithLifecycle()
    val dupe by vm.dupe.collectAsStateWithLifecycle()
    val editing = editingId != 0L
    val scrollBehavior = TopAppBarDefaults.pinnedScrollBehavior()

    Scaffold(
        modifier = Modifier.nestedScroll(scrollBehavior.nestedScrollConnection),
        topBar = {
            TopAppBar(
                title = { Text(if (editing) "Edit QSO" else "New QSO") },
                scrollBehavior = scrollBehavior,
                navigationIcon = {
                    IconButton(onClick = { vm.cancel(); nav.popBackStack() }) {
                        Icon(Icons.Default.Close, contentDescription = "Cancel")
                    }
                },
            )
        },
    ) { pad ->
        Column(
            Modifier
                .padding(pad)
                .fillMaxSize()
                .imePadding()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 16.dp, vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
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
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(
                        Icons.Outlined.Public,
                        contentDescription = null,
                        modifier = Modifier.size(18.dp),
                        tint = MaterialTheme.colorScheme.secondary,
                    )
                    Spacer(Modifier.size(6.dp))
                    Text(
                        dxcc,
                        color = MaterialTheme.colorScheme.secondary,
                        style = MaterialTheme.typography.bodyMedium,
                    )
                }
            }
            if (dupe.isNotEmpty()) {
                Surface(
                    shape = MaterialTheme.shapes.medium,
                    color = MaterialTheme.colorScheme.errorContainer,
                    contentColor = MaterialTheme.colorScheme.onErrorContainer,
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Row(
                        Modifier.padding(12.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Icon(
                            Icons.Outlined.Warning,
                            contentDescription = null,
                            modifier = Modifier.size(20.dp),
                        )
                        Spacer(Modifier.size(8.dp))
                        Text(dupe, style = MaterialTheme.typography.bodyMedium)
                    }
                }
            }

            SectionCard("Signal") {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
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
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Field("RST s", form.rstSent, Modifier.weight(1f)) { vm.update { q -> q.copy(rstSent = it) } }
                    Field("RST r", form.rstRcvd, Modifier.weight(1f)) { vm.update { q -> q.copy(rstRcvd = it) } }
                    Field("Power", form.power, Modifier.weight(1f), KeyboardType.Number) {
                        vm.update { q -> q.copy(power = it) }
                    }
                }
            }

            SectionCard("Operator") {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Field("Name", form.name, Modifier.weight(1f)) { vm.update { q -> q.copy(name = it) } }
                    Field("Grid", form.locator, Modifier.weight(1f)) {
                        vm.update { q -> q.copy(locator = it.uppercase()) }
                    }
                }
                Field("QTH", form.qth, Modifier.fillMaxWidth()) { vm.update { q -> q.copy(qth = it) } }
                Field("Comment", form.comment, Modifier.fillMaxWidth()) { vm.update { q -> q.copy(comment = it) } }
            }

            Button(
                onClick = { if (vm.save()) nav.popBackStack() },
                modifier = Modifier.fillMaxWidth().padding(top = 4.dp, bottom = 12.dp),
            ) {
                Text(if (editing) "UPDATE" else "LOG", fontSize = 18.sp)
            }
        }
    }
}

@Composable
private fun SectionCard(title: String, content: @Composable () -> Unit) {
    ElevatedCard(Modifier.fillMaxWidth()) {
        Column(
            Modifier.fillMaxWidth().padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text(
                title,
                style = MaterialTheme.typography.titleSmall,
                color = MaterialTheme.colorScheme.primary,
            )
            content()
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
