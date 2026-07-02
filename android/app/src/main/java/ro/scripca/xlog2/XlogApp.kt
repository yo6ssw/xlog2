// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

package ro.scripca.xlog2

import android.app.Application

class XlogApp : Application() {
    override fun onCreate() {
        super.onCreate()
        // Eagerly create the repository singleton so the service + UI share it.
        XlogRepository.get(this)
    }
}
