package ro.scripca.xlog2

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Sync
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Card
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
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
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.navigation.NavHostController
import kotlinx.coroutines.launch

/**
 * The home screen: the QSO log, newest first. A + button opens the entry screen
 * for a new QSO; tapping a row edits it; long-pressing a row offers to delete it.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun LogListScreen(nav: NavHostController, vm: LogViewModel) {
    val qsos by vm.qsos.collectAsStateWithLifecycle()
    val members by vm.memberCount.collectAsStateWithLifecycle()
    val newestFirst = remember(qsos) { qsos.asReversed() }

    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    var menuOpen by remember { mutableStateOf(false) }
    var pendingDelete by remember { mutableStateOf<Qso?>(null) }

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
        floatingActionButton = {
            FloatingActionButton(onClick = {
                vm.startAdd()
                nav.navigate(Routes.ENTRY)
            }) {
                Icon(Icons.Default.Add, contentDescription = "Add QSO")
            }
        },
    ) { pad ->
        if (newestFirst.isEmpty()) {
            Box(Modifier.padding(pad).fillMaxSize(), contentAlignment = Alignment.Center) {
                Text(
                    "No QSOs yet — tap + to log one.",
                    style = MaterialTheme.typography.bodyLarge,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        } else {
            LazyColumn(
                Modifier.padding(pad).fillMaxSize().padding(horizontal = 12.dp),
            ) {
                items(newestFirst, key = { it.id }) { q ->
                    QsoRow(
                        q = q,
                        onClick = { vm.startEdit(q); nav.navigate(Routes.ENTRY) },
                        onLongClick = { pendingDelete = q },
                    )
                }
            }
        }
    }

    pendingDelete?.let { q ->
        AlertDialog(
            onDismissRequest = { pendingDelete = null },
            title = { Text("Delete QSO?") },
            text = {
                Text(buildString {
                    append(q.call)
                    if (q.band.isNotEmpty() || q.mode.isNotEmpty())
                        append(" on ${q.band} ${q.mode}".trimEnd())
                    append("  ·  ${q.date} ${q.timeOn}")
                })
            },
            confirmButton = {
                TextButton(onClick = { vm.delete(q); pendingDelete = null }) {
                    Text("Delete", color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = {
                TextButton(onClick = { pendingDelete = null }) { Text("Cancel") }
            },
        )
    }
}

@OptIn(ExperimentalFoundationApi::class, ExperimentalMaterial3Api::class)
@Composable
private fun QsoRow(q: Qso, onClick: () -> Unit, onLongClick: () -> Unit) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 3.dp)
            .combinedClickable(onClick = onClick, onLongClick = onLongClick),
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
