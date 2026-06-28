package ro.scripca.xlog2

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

// A restrained, field-ready palette: deep slate + a signal amber accent, so the
// LOG action and live indicators read at a glance outdoors. Not the Compose
// purple defaults.
private val Amber = Color(0xFFFFB300)
private val Slate = Color(0xFF1F2933)

private val DarkColors = darkColorScheme(
    primary = Amber,
    onPrimary = Color(0xFF1A1300),
    secondary = Color(0xFF7FB2D6),
    background = Color(0xFF121821),
    surface = Slate,
)

private val LightColors = lightColorScheme(
    primary = Color(0xFFB07400),
    onPrimary = Color.White,
    secondary = Color(0xFF2A6F9E),
)

@Composable
fun XlogTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = if (isSystemInDarkTheme()) DarkColors else LightColors,
        content = content,
    )
}
