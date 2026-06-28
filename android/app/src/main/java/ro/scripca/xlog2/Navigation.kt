package ro.scripca.xlog2

import androidx.compose.runtime.Composable
import androidx.navigation.NavHostController
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable

object Routes {
    const val LOG = "log"
    const val SETTINGS = "settings"
    const val SYNC = "sync"
    const val PEERS = "peers"
}

@Composable
fun XlogNavHost(nav: NavHostController) {
    NavHost(navController = nav, startDestination = Routes.LOG) {
        composable(Routes.LOG) { LogScreen(nav) }
        composable(Routes.SETTINGS) { SettingsScreen(nav) }
        composable(Routes.SYNC) { SyncScreen(nav) }
        composable(Routes.PEERS) { PeersScreen(nav) }
    }
}
