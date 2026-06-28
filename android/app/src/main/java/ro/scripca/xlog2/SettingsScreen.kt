package ro.scripca.xlog2

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
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

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Settings") },
                navigationIcon = {
                    IconButton(onClick = { nav.popBackStack() }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
            )
        },
    ) { pad ->
        Column(
            Modifier.padding(pad).fillMaxSize().padding(16.dp).verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Section("Station")
            OutlinedTextField(call, { call = it.uppercase() }, label = { Text("My callsign") },
                singleLine = true, modifier = Modifier.fillMaxWidth())
            OutlinedTextField(locator, { locator = it.uppercase() }, label = { Text("My locator (grid)") },
                singleLine = true, modifier = Modifier.fillMaxWidth())

            Section("Sync mesh")
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Enabled", Modifier.weight(1f))
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

            Section("QRZ.com (optional)")
            OutlinedTextField(qrzUser, { qrzUser = it }, label = { Text("QRZ username") },
                singleLine = true, modifier = Modifier.fillMaxWidth())
            OutlinedTextField(qrzPass, { qrzPass = it }, label = { Text("QRZ password") },
                singleLine = true, visualTransformation = PasswordVisualTransformation(),
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                modifier = Modifier.fillMaxWidth())

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
                    // Re-open the core with the new secret/peers/creds, then
                    // (re)align the foreground service with the enabled toggle.
                    XlogRepository.get(ctx).restart()
                    if (enabled) SyncService.start(ctx) else SyncService.stop(ctx)
                    nav.popBackStack()
                },
                modifier = Modifier.fillMaxWidth().padding(top = 8.dp),
            ) { Text("Save & restart sync") }
        }
    }
}

@Composable
private fun Section(title: String) {
    Text(title, style = androidx.compose.material3.MaterialTheme.typography.titleMedium,
        modifier = Modifier.padding(top = 8.dp))
}
