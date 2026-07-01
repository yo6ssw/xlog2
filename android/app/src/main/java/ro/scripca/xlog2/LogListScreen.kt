package ro.scripca.xlog2

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.combinedClickable
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
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Headphones
import androidx.compose.material.icons.filled.Podcasts
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Sync
import androidx.compose.material.icons.outlined.Inbox
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.BadgedBox
import androidx.compose.material3.Badge
import androidx.compose.material3.Card
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExtendedFloatingActionButton
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
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.nestedscroll.nestedScroll
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
    val scrollBehavior = TopAppBarDefaults.enterAlwaysScrollBehavior()

    // SAF pickers for ADIF import/export (bytes ⇄ native Adif.cpp).
    val importLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri -> if (uri != null) scope.launch { AdifIo.import(ctx, uri) } }
    val exportLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("application/octet-stream")
    ) { uri -> if (uri != null) scope.launch { AdifIo.export(ctx, uri) } }

    Scaffold(
        modifier = Modifier.nestedScroll(scrollBehavior.nestedScrollConnection),
        topBar = {
            TopAppBar(
                title = { Text("XLOG2", fontWeight = FontWeight.Bold) },
                scrollBehavior = scrollBehavior,
                actions = {
                    IconButton(onClick = { nav.navigate(Routes.SYNC) }) {
                        BadgedBox(
                            badge = {
                                if (members > 0) Badge { Text(members.toString()) }
                            }
                        ) {
                            Icon(Icons.Default.Sync, contentDescription = "Sync ($members peers)")
                        }
                    }
                    IconButton(onClick = { nav.navigate(Routes.AUDIO) }) {
                        Icon(Icons.Default.Headphones, contentDescription = "Rig audio")
                    }
                    IconButton(onClick = { nav.navigate(Routes.PADDLE) }) {
                        Icon(Icons.Default.Podcasts, contentDescription = "CW paddle keyer")
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
            ExtendedFloatingActionButton(
                onClick = {
                    vm.startAdd()
                    nav.navigate(Routes.ENTRY)
                },
                icon = { Icon(Icons.Default.Add, contentDescription = null) },
                text = { Text("Log QSO") },
            )
        },
    ) { pad ->
        if (newestFirst.isEmpty()) {
            EmptyLog(Modifier.padding(pad))
        } else {
            LazyColumn(
                modifier = Modifier.padding(pad).fillMaxSize(),
                contentPadding = PaddingValues(horizontal = 12.dp, vertical = 8.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
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

@Composable
private fun EmptyLog(modifier: Modifier = Modifier) {
    Box(modifier.fillMaxSize().padding(24.dp), contentAlignment = Alignment.Center) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Icon(
                Icons.Outlined.Inbox,
                contentDescription = null,
                modifier = Modifier.size(72.dp),
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(Modifier.size(16.dp))
            Text(
                "No QSOs yet",
                style = MaterialTheme.typography.titleMedium,
            )
            Spacer(Modifier.size(4.dp))
            Text(
                "Tap “Log QSO” to record your first contact.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun QsoRow(q: Qso, onClick: () -> Unit, onLongClick: () -> Unit) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .combinedClickable(onClick = onClick, onLongClick = onLongClick),
    ) {
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            // Leading band badge — the at-a-glance "what band" cue.
            Surface(
                shape = MaterialTheme.shapes.small,
                color = MaterialTheme.colorScheme.primaryContainer,
                contentColor = MaterialTheme.colorScheme.onPrimaryContainer,
            ) {
                Box(
                    Modifier.widthIn(min = 52.dp).padding(horizontal = 8.dp, vertical = 6.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(
                        q.band.ifEmpty { "—" },
                        style = MaterialTheme.typography.labelLarge,
                        fontWeight = FontWeight.Bold,
                        maxLines = 1,
                    )
                }
            }

            Spacer(Modifier.size(12.dp))

            Column(Modifier.weight(1f)) {
                Text(
                    q.call,
                    fontFamily = FontFamily.Monospace,
                    fontWeight = FontWeight.Bold,
                    style = MaterialTheme.typography.titleMedium,
                )
                val sub = listOf(q.name, q.country).filter { it.isNotEmpty() }.joinToString(" · ")
                if (sub.isNotEmpty()) {
                    Text(
                        sub,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }

            Column(
                horizontalAlignment = Alignment.End,
                modifier = Modifier.padding(start = 8.dp),
            ) {
                if (q.mode.isNotEmpty()) {
                    Text(
                        q.mode,
                        style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.secondary,
                        fontWeight = FontWeight.SemiBold,
                    )
                }
                Text(
                    q.date,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Text(
                    q.timeOn,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}
