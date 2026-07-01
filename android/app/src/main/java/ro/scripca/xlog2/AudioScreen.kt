package ro.scripca.xlog2

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ElevatedCard
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.navigation.NavHostController

/**
 * Rig-audio panel: a big live frequency readout (polled from rigctld) and a
 * play/stop control for the cwsd Opus stream. Play/stop go through
 * [AudioService] (a mediaPlayback foreground service) so audio keeps running
 * with the screen off; the readout mirrors [XlogRepository]'s flows.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AudioScreen(nav: NavHostController) {
    val ctx = LocalContext.current
    val repo = XlogRepository.get(ctx)
    val settings = remember(ctx) { Settings(ctx) }

    val active by repo.audioActive.collectAsStateWithLifecycle()
    val streaming by repo.audioStreaming.collectAsStateWithLifecycle()
    val status by repo.audioStatus.collectAsStateWithLifecycle()
    val frames by repo.audioFrames.collectAsStateWithLifecycle()
    val freqHz by repo.rigFreqHz.collectAsStateWithLifecycle()
    val rigConnected by repo.rigConnected.collectAsStateWithLifecycle()

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Rig audio") },
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
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            // --- frequency readout -------------------------------------------
            ElevatedCard(Modifier.fillMaxWidth()) {
                Column(
                    Modifier.fillMaxWidth().padding(24.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    Text(
                        "FREQUENCY",
                        style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.primary,
                    )
                    Spacer(Modifier.height(8.dp))
                    Text(
                        text = freqHz?.let { formatMhz(it) } ?: "— — —",
                        fontFamily = FontFamily.Monospace,
                        fontWeight = FontWeight.Bold,
                        fontSize = 44.sp,
                        textAlign = TextAlign.Center,
                        color = MaterialTheme.colorScheme.onSurface,
                    )
                    Text(
                        "MHz",
                        style = MaterialTheme.typography.titleMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Spacer(Modifier.height(8.dp))
                    when {
                        !settings.freqEnabled ->
                            StatusLine("Frequency polling off (enable in Settings)", Color.Gray)
                        !active ->
                            StatusLine("Press Play to show frequency", Color.Gray)
                        rigConnected ->
                            StatusLine("rigctld connected", Color(0xFF2E7D32))
                        else ->
                            StatusLine("rigctld: connecting…", Color(0xFFB26A00))
                    }
                }
            }

            // --- play / stop --------------------------------------------------
            Button(
                onClick = {
                    if (active) AudioService.stop(ctx) else AudioService.start(ctx)
                },
                modifier = Modifier.fillMaxWidth().height(56.dp),
                colors = if (active)
                    ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.error)
                else ButtonDefaults.buttonColors(),
            ) {
                Icon(if (active) Icons.Default.Stop else Icons.Default.PlayArrow, contentDescription = null)
                Spacer(Modifier.size(8.dp))
                Text(if (active) "STOP" else "PLAY", fontWeight = FontWeight.Bold, fontSize = 18.sp)
            }

            // --- stream status ------------------------------------------------
            ElevatedCard(Modifier.fillMaxWidth()) {
                Column(
                    Modifier.fillMaxWidth().padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Surface(
                            color = if (streaming) Color(0xFF2E7D32) else Color.Gray,
                            shape = CircleShape,
                            modifier = Modifier.size(10.dp),
                        ) {}
                        Spacer(Modifier.size(8.dp))
                        Text(
                            if (streaming) "Streaming from ${settings.audioHost}:${settings.audioPort}"
                            else "Stopped",
                            style = MaterialTheme.typography.bodyLarge,
                        )
                    }
                    Text(
                        "${settings.audioSampleRate} Hz · ${if (settings.audioChannels >= 2) "stereo" else "mono"} · $frames frames decoded",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    if (status.isNotEmpty()) {
                        Text(
                            status,
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun StatusLine(text: String, color: Color) {
    Text(text, style = MaterialTheme.typography.bodySmall, color = color)
}

/** 14074000 Hz → "14.07400" MHz string (10 Hz resolution). */
private fun formatMhz(hz: Long): String {
    val mhz = hz / 1_000_000L
    val rest = (hz % 1_000_000L) / 10L   // 5 fractional digits, 10 Hz resolution
    return "%d.%05d".format(mhz, rest)
}
