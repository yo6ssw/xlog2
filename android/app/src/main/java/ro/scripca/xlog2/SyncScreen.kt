package ro.scripca.xlog2

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.navigation.NavHostController

/**
 * Sync status: this node's id + identity key (to paste into a desktop peer's
 * trusted list), member count, Sync-now, and a route to the trusted-peers list.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SyncScreen(nav: NavHostController) {
    val ctx = LocalContext.current
    val repo = XlogRepository.get(ctx)
    val members by repo.memberCount.collectAsStateWithLifecycle()
    val localId by repo.localId.collectAsStateWithLifecycle()
    val identity by repo.identityKey.collectAsStateWithLifecycle()

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Sync") },
                navigationIcon = {
                    IconButton(onClick = { nav.popBackStack() }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
            )
        },
    ) { pad ->
        Column(
            Modifier.padding(pad).fillMaxSize().padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text("$members reachable peer(s)", style = MaterialTheme.typography.titleMedium)
                }
            }

            KeyCard("This node id", localId)
            KeyCard("Identity key (add to a desktop peer's trusted list)", identity)

            Button(onClick = { repo.syncNow() }, modifier = Modifier.fillMaxWidth()) {
                Text("Sync now")
            }
            OutlinedButton(onClick = { nav.navigate(Routes.PEERS) }, modifier = Modifier.fillMaxWidth()) {
                Text("Trusted peers")
            }
        }
    }
}

@Composable
private fun KeyCard(label: String, value: String) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(label, style = MaterialTheme.typography.labelMedium)
            Text(
                value.ifEmpty { "—" },
                fontFamily = FontFamily.Monospace,
                style = MaterialTheme.typography.bodySmall,
            )
        }
    }
}
