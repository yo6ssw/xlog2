// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

package ro.scripca.xlog2

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.waitForUpOrCancellation
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.ElevatedCard
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.navigation.NavHostController

/**
 * CW paddle-keyer panel: an on/off control (start/stop the background keyer +
 * USB paddle), live keyer/USB/transmit status, and on-screen dit/dah keys so the
 * keyer can be exercised without the USB paddle. Keying config lives in Settings.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PaddleScreen(nav: NavHostController) {
    val ctx = LocalContext.current
    val repo = XlogRepository.get(ctx)
    val settings = remember(ctx) { Settings(ctx) }

    val active by repo.paddleActive.collectAsStateWithLifecycle()
    val transmitting by repo.paddleTransmitting.collectAsStateWithLifecycle()
    val keyerStatus by repo.paddleStatus.collectAsStateWithLifecycle()
    val usbStatus by repo.usbPaddleStatus.collectAsStateWithLifecycle()
    val usbConnected by repo.usbPaddleConnected.collectAsStateWithLifecycle()

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("CW paddle keyer") },
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
        ) {
            // --- on/off + transmit indicator --------------------------------
            ElevatedCard(Modifier.fillMaxWidth()) {
                Column(
                    Modifier.fillMaxWidth().padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text("Keyer", style = MaterialTheme.typography.titleMedium)
                            Text(
                                "${settings.paddleWpm} wpm · ${settings.effectivePaddleHost}:${settings.paddlePort}",
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                        Switch(
                            checked = active,
                            onCheckedChange = {
                                if (it) PaddleService.start(ctx) else PaddleService.stop(ctx)
                            },
                        )
                    }
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Dot(if (transmitting) Color(0xFFD32F2F) else Color.Gray)
                        Spacer(Modifier.size(8.dp))
                        Text(
                            if (transmitting) "TRANSMIT" else "idle",
                            fontWeight = FontWeight.Bold,
                            color = if (transmitting) Color(0xFFD32F2F)
                            else MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }

            // --- statuses ----------------------------------------------------
            ElevatedCard(Modifier.fillMaxWidth()) {
                Column(
                    Modifier.fillMaxWidth().padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Dot(if (usbConnected) Color(0xFF2E7D32) else Color.Gray)
                        Spacer(Modifier.size(8.dp))
                        Text(usbStatus, style = MaterialTheme.typography.bodyMedium)
                    }
                    if (keyerStatus.isNotEmpty()) {
                        Text(
                            keyerStatus,
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }

            // --- on-screen paddle (touch to test without USB) ----------------
            Text(
                "Touch keys (or use the USB paddle)",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.primary,
            )
            Row(
                Modifier.fillMaxWidth().weight(1f),
                horizontalArrangement = Arrangement.spacedBy(16.dp),
            ) {
                PaddleKey("DIT  ·", Modifier.weight(1f), active) { repo.paddleDit(it) }
                PaddleKey("·  DAH", Modifier.weight(1f), active) { repo.paddleDah(it) }
            }
        }
    }
}

@Composable
private fun Dot(color: Color) {
    Surface(color = color, shape = CircleShape, modifier = Modifier.size(12.dp)) {}
}

/** A press-and-hold key: reports true on press-down and false on release. */
@Composable
private fun PaddleKey(
    label: String,
    modifier: Modifier = Modifier,
    enabled: Boolean,
    onPress: (Boolean) -> Unit,
) {
    val container =
        if (enabled) MaterialTheme.colorScheme.primaryContainer
        else MaterialTheme.colorScheme.surfaceVariant
    Surface(
        color = container,
        shape = RoundedCornerShape(16.dp),
        modifier = modifier
            .fillMaxSize()
            .then(
                if (enabled) Modifier.pointerInput(Unit) {
                    awaitPointerEventScope {
                        while (true) {
                            awaitFirstDown(requireUnconsumed = false)
                            onPress(true)
                            waitForUpOrCancellation()   // returns on release or cancel
                            onPress(false)
                        }
                    }
                } else Modifier
            ),
    ) {
        Box(contentAlignment = Alignment.Center) {
            Text(label, fontWeight = FontWeight.Bold, fontSize = 22.sp)
        }
    }
}
