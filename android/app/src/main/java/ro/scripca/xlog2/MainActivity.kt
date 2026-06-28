package ro.scripca.xlog2

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import androidx.navigation.compose.rememberNavController

class MainActivity : ComponentActivity() {

    private val requestNotif = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { /* notification permission is best-effort */ }

    override fun onCreate(savedInstanceState: Bundle?) {
        // Draw behind the system bars (Android 15 enforces this on targetSdk 35);
        // the bar icons auto-adapt to light/dark and Scaffold supplies the insets.
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
            != PackageManager.PERMISSION_GRANTED
        ) {
            requestNotif.launch(Manifest.permission.POST_NOTIFICATIONS)
        }

        // The repository owns the core (logbook + mesh) for the whole process,
        // so logging works regardless of sync. The foreground service only adds
        // background liveness + the MulticastLock when sync is enabled.
        XlogRepository.get(this).start()
        if (Settings(this).syncEnabled) SyncService.start(this)

        setContent {
            XlogTheme {
                val nav = rememberNavController()
                XlogNavHost(nav)
            }
        }
    }
}
