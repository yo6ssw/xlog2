package ro.scripca.xlog2

import android.app.Application

class XlogApp : Application() {
    override fun onCreate() {
        super.onCreate()
        // Eagerly create the repository singleton so the service + UI share it.
        XlogRepository.get(this)
    }
}
