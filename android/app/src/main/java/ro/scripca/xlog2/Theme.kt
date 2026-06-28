package ro.scripca.xlog2

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext

// Brand identity: a signal-amber accent over deep slate, chosen so the primary
// LOG action and the live sync/decode indicators read at a glance outdoors —
// not the Compose purple defaults. On Material You devices (Android 12+) we
// defer to the wallpaper-derived palette and fall back to this hand-tuned,
// fully-roled scheme everywhere else.

private val BrandLight = lightColorScheme(
    primary = Color(0xFF8A5300),
    onPrimary = Color(0xFFFFFFFF),
    primaryContainer = Color(0xFFFFDDB3),
    onPrimaryContainer = Color(0xFF2C1700),
    secondary = Color(0xFF00658E),
    onSecondary = Color(0xFFFFFFFF),
    secondaryContainer = Color(0xFFC7E7FF),
    onSecondaryContainer = Color(0xFF001E2E),
    tertiary = Color(0xFF4F6352),
    onTertiary = Color(0xFFFFFFFF),
    tertiaryContainer = Color(0xFFD1E8D4),
    onTertiaryContainer = Color(0xFF0C1F12),
    background = Color(0xFFFFF8F4),
    onBackground = Color(0xFF211A13),
    surface = Color(0xFFFFF8F4),
    onSurface = Color(0xFF211A13),
    surfaceVariant = Color(0xFFF1E0CF),
    onSurfaceVariant = Color(0xFF504539),
    outline = Color(0xFF827568),
    outlineVariant = Color(0xFFD4C4B4),
    error = Color(0xFFBA1A1A),
    onError = Color(0xFFFFFFFF),
    errorContainer = Color(0xFFFFDAD6),
    onErrorContainer = Color(0xFF410002),
)

private val BrandDark = darkColorScheme(
    primary = Color(0xFFFFB951),
    onPrimary = Color(0xFF492900),
    primaryContainer = Color(0xFF693C00),
    onPrimaryContainer = Color(0xFFFFDDB3),
    secondary = Color(0xFF87CFFF),
    onSecondary = Color(0xFF00344C),
    secondaryContainer = Color(0xFF004C6C),
    onSecondaryContainer = Color(0xFFC7E7FF),
    tertiary = Color(0xFFB6CCB8),
    onTertiary = Color(0xFF223526),
    tertiaryContainer = Color(0xFF384B3B),
    onTertiaryContainer = Color(0xFFD1E8D4),
    background = Color(0xFF121821),
    onBackground = Color(0xFFE9E1D9),
    surface = Color(0xFF161D27),
    onSurface = Color(0xFFE9E1D9),
    surfaceVariant = Color(0xFF504539),
    onSurfaceVariant = Color(0xFFD4C4B4),
    outline = Color(0xFF9D8F80),
    outlineVariant = Color(0xFF504539),
    error = Color(0xFFFFB4AB),
    onError = Color(0xFF690005),
    errorContainer = Color(0xFF93000A),
    onErrorContainer = Color(0xFFFFDAD6),
)

@Composable
fun XlogTheme(
    darkTheme: Boolean = isSystemInDarkTheme(),
    dynamicColor: Boolean = true,
    content: @Composable () -> Unit,
) {
    val colors = when {
        dynamicColor && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S -> {
            val ctx = LocalContext.current
            if (darkTheme) dynamicDarkColorScheme(ctx) else dynamicLightColorScheme(ctx)
        }
        darkTheme -> BrandDark
        else -> BrandLight
    }
    MaterialTheme(colorScheme = colors, content = content)
}
