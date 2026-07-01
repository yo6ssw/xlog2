package ro.scripca.xlog2

import androidx.compose.runtime.Composable
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.NavHostController
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable

object Routes {
    const val LOG = "log"
    const val ENTRY = "entry"
    const val SETTINGS = "settings"
    const val SYNC = "sync"
    const val PEERS = "peers"
    const val AUDIO = "audio"
    const val PADDLE = "paddle"
}

@Composable
fun XlogNavHost(nav: NavHostController) {
    // One LogViewModel shared by the list and the add/edit screen. Obtained here
    // (outside the NavHost destinations) so it's scoped to the Activity rather
    // than a per-destination NavBackStackEntry — both screens see the same form
    // and logbook state.
    val logVm: LogViewModel = viewModel()

    NavHost(navController = nav, startDestination = Routes.LOG) {
        composable(Routes.LOG) { LogListScreen(nav, logVm) }
        composable(Routes.ENTRY) { EntryScreen(nav, logVm) }
        composable(Routes.SETTINGS) { SettingsScreen(nav) }
        composable(Routes.SYNC) { SyncScreen(nav) }
        composable(Routes.PEERS) { PeersScreen(nav) }
        composable(Routes.AUDIO) { AudioScreen(nav) }
        composable(Routes.PADDLE) { PaddleScreen(nav) }
    }
}
