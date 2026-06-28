package ro.scripca.xlog2

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.outlined.Group
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.navigation.NavHostController

/**
 * Trusted-peers management. Every identity-verified peer shows up here (online or
 * trusted-but-offline); the operator admits or revokes each one. An untrusted
 * peer exchanges no logbook data until admitted.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PeersScreen(nav: NavHostController) {
    val ctx = LocalContext.current
    val repo = XlogRepository.get(ctx)
    val peers by repo.peers.collectAsStateWithLifecycle()
    val scrollBehavior = TopAppBarDefaults.enterAlwaysScrollBehavior()

    Scaffold(
        modifier = Modifier.nestedScroll(scrollBehavior.nestedScrollConnection),
        topBar = {
            TopAppBar(
                title = { Text("Trusted peers") },
                scrollBehavior = scrollBehavior,
                navigationIcon = {
                    IconButton(onClick = { nav.popBackStack() }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
            )
        },
    ) { pad ->
        if (peers.isEmpty()) {
            Box(Modifier.padding(pad).fillMaxSize().padding(24.dp), contentAlignment = Alignment.Center) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Icon(
                        Icons.Outlined.Group,
                        contentDescription = null,
                        modifier = Modifier.size(72.dp),
                        tint = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Spacer(Modifier.size(16.dp))
                    Text("No peers discovered yet", style = MaterialTheme.typography.titleMedium)
                    Spacer(Modifier.size(4.dp))
                    Text(
                        "Make sure another node shares the same secret and is on the same Wi-Fi, or add a static WAN peer in Settings.",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
            return@Scaffold
        }
        LazyColumn(
            modifier = Modifier.padding(pad).fillMaxSize(),
            contentPadding = PaddingValues(12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            items(peers, key = { it.id }) { p ->
                PeerCard(
                    p,
                    onTrust = { repo.trustPeer(p.id) },
                    onRevoke = { repo.revokePeer(p.id) },
                )
            }
        }
    }
}

@Composable
private fun PeerCard(p: PeerInfo, onTrust: () -> Unit, onRevoke: () -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Row(
            Modifier.fillMaxWidth().padding(start = 16.dp, top = 12.dp, bottom = 12.dp, end = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            // Online/offline dot.
            Surface(
                shape = CircleShape,
                color = if (p.online) MaterialTheme.colorScheme.tertiary
                        else MaterialTheme.colorScheme.outlineVariant,
                modifier = Modifier.size(10.dp),
            ) {}
            Spacer(Modifier.size(12.dp))
            Column(Modifier.weight(1f)) {
                Text(p.name.ifEmpty { "(unnamed)" }, style = MaterialTheme.typography.titleSmall)
                Text(
                    p.id,
                    fontFamily = FontFamily.Monospace,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                val state = buildList {
                    if (p.online) add("online") else add("offline")
                    if (p.ready) add("syncing")
                    if (p.trusted) add("trusted")
                }.joinToString(" · ")
                Text(
                    state,
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.secondary,
                )
            }
            Spacer(Modifier.size(8.dp))
            if (p.trusted) {
                TextButton(onClick = onRevoke) {
                    Text("Revoke", color = MaterialTheme.colorScheme.error)
                }
            } else {
                FilledTonalButton(onClick = onTrust) { Text("Trust") }
            }
        }
    }
}
