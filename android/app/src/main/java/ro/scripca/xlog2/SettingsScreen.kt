package ro.scripca.xlog2

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Button
import androidx.compose.material3.ElevatedCard
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.navigation.NavHostController

/**
 * Station + sync + QRZ settings. Saving requires the mesh to restart with the
 * new secret/peers, so we stop+start the service on save.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(nav: NavHostController) {
    val ctx = LocalContext.current
    val s = remember { Settings(ctx) }

    var call by remember { mutableStateOf(s.myCallsign) }
    var locator by remember { mutableStateOf(s.myLocator) }
    var enabled by remember { mutableStateOf(s.syncEnabled) }
    var secret by remember { mutableStateOf(s.syncSecret) }
    var nodeName by remember { mutableStateOf(s.syncNodeName) }
    var peers by remember { mutableStateOf(s.staticPeers.joinToString("\n")) }
    var qrzUser by remember { mutableStateOf(s.qrzUser) }
    var qrzPass by remember { mutableStateOf(s.qrzPassword) }
    var audioHost by remember { mutableStateOf(s.audioHost) }
    var audioPort by remember { mutableStateOf(s.audioPort.toString()) }
    var audioRate by remember { mutableStateOf(s.audioSampleRate.toString()) }
    var audioChannels by remember { mutableStateOf(s.audioChannels.toString()) }
    var freqEnabled by remember { mutableStateOf(s.freqEnabled) }
    var freqHost by remember { mutableStateOf(s.freqHost) }
    var freqPort by remember { mutableStateOf(s.freqPort.toString()) }
    val scrollBehavior = TopAppBarDefaults.pinnedScrollBehavior()

    Scaffold(
        modifier = Modifier.nestedScroll(scrollBehavior.nestedScrollConnection),
        topBar = {
            TopAppBar(
                title = { Text("Settings") },
                scrollBehavior = scrollBehavior,
                navigationIcon = {
                    IconButton(onClick = { nav.popBackStack() }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
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
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            SectionCard("Station") {
                OutlinedTextField(call, { call = it.uppercase() }, label = { Text("My callsign") },
                    singleLine = true, modifier = Modifier.fillMaxWidth())
                OutlinedTextField(locator, { locator = it.uppercase() }, label = { Text("My locator (grid)") },
                    singleLine = true, modifier = Modifier.fillMaxWidth())
            }

            SectionCard("Sync mesh") {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text("Enabled", style = MaterialTheme.typography.bodyLarge)
                        Text(
                            "Keep the logbook in sync with your other devices",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    Switch(checked = enabled, onCheckedChange = { enabled = it })
                }
                OutlinedTextField(secret, { secret = it }, label = { Text("Shared secret (PSK)") },
                    singleLine = true, visualTransformation = PasswordVisualTransformation(),
                    modifier = Modifier.fillMaxWidth())
                OutlinedTextField(nodeName, { nodeName = it }, label = { Text("Node name (optional)") },
                    singleLine = true, modifier = Modifier.fillMaxWidth())
                OutlinedTextField(peers, { peers = it },
                    label = { Text("Static WAN peers (one host[:port] per line)") },
                    modifier = Modifier.fillMaxWidth())
            }

            SectionCard("QRZ.com (optional)") {
                OutlinedTextField(qrzUser, { qrzUser = it }, label = { Text("QRZ username") },
                    singleLine = true, modifier = Modifier.fillMaxWidth())
                OutlinedTextField(qrzPass, { qrzPass = it }, label = { Text("QRZ password") },
                    singleLine = true, visualTransformation = PasswordVisualTransformation(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                    modifier = Modifier.fillMaxWidth())
            }

            SectionCard("Rig audio (cwsd Opus stream)") {
                OutlinedTextField(audioHost, { audioHost = it }, label = { Text("Audio host") },
                    singleLine = true, modifier = Modifier.fillMaxWidth())
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedTextField(audioPort, { audioPort = it.filter(Char::isDigit) },
                        label = { Text("UDP port") }, singleLine = true,
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                        modifier = Modifier.weight(1f))
                    OutlinedTextField(audioRate, { audioRate = it.filter(Char::isDigit) },
                        label = { Text("Sample rate") }, singleLine = true,
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                        modifier = Modifier.weight(1f))
                    OutlinedTextField(audioChannels, { audioChannels = it.filter(Char::isDigit) },
                        label = { Text("Channels") }, singleLine = true,
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                        modifier = Modifier.weight(1f))
                }
                Text(
                    "Sample rate and channels must match the server.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text("Show rig frequency", style = MaterialTheme.typography.bodyLarge)
                        Text(
                            "Poll a Hamlib rigctld server for the current frequency",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    Switch(checked = freqEnabled, onCheckedChange = { freqEnabled = it })
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedTextField(freqHost, { freqHost = it },
                        label = { Text("rigctld host (blank = audio host)") }, singleLine = true,
                        modifier = Modifier.weight(2f))
                    OutlinedTextField(freqPort, { freqPort = it.filter(Char::isDigit) },
                        label = { Text("Port") }, singleLine = true,
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                        modifier = Modifier.weight(1f))
                }
            }

            Button(
                onClick = {
                    s.myCallsign = call
                    s.myLocator = locator
                    s.syncEnabled = enabled
                    s.syncSecret = secret
                    s.syncNodeName = nodeName
                    s.staticPeers = peers.split('\n').map { it.trim() }.filter { it.isNotEmpty() }
                    s.qrzUser = qrzUser
                    s.qrzPassword = qrzPass
                    s.audioHost = audioHost.trim()
                    s.audioPort = audioPort.toIntOrNull() ?: 7355
                    s.audioSampleRate = audioRate.toIntOrNull() ?: 8000
                    s.audioChannels = audioChannels.toIntOrNull() ?: 1
                    s.freqEnabled = freqEnabled
                    s.freqHost = freqHost.trim()
                    s.freqPort = freqPort.toIntOrNull() ?: 4532
                    // Re-open the core with the new secret/peers/creds, then
                    // (re)align the foreground service with the enabled toggle.
                    XlogRepository.get(ctx).restart()
                    if (enabled) SyncService.start(ctx) else SyncService.stop(ctx)
                    nav.popBackStack()
                },
                modifier = Modifier.fillMaxWidth().padding(top = 4.dp),
            ) { Text("Save & restart sync") }

            Text(
                "XLOG2  ·  v${BuildConfig.VERSION_NAME} (build ${BuildConfig.VERSION_CODE})",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                textAlign = TextAlign.Center,
                modifier = Modifier.fillMaxWidth().padding(top = 8.dp, bottom = 12.dp),
            )
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
