// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

package ro.scripca.xlog2

import android.content.ContentValues
import android.content.Context
import android.database.sqlite.SQLiteDatabase
import android.database.sqlite.SQLiteOpenHelper

/**
 * A tiny on-device cache of qrz.com lookups so repeated calls don't re-hit the
 * network and resolve offline. Mirrors the desktop native schema
 * (`src/core/services/QrzCache.cpp`), but in its own file so the formats stay
 * decoupled — the native QRZ subsystem is compiled out of the mobile build.
 *
 * Lifetime is [maxAgeDays] (default 365, matching the desktop default); a stale
 * row is treated as a miss.
 */
class QrzCache(ctx: Context) : SQLiteOpenHelper(ctx, DB_NAME, null, 1) {

    override fun onCreate(db: SQLiteDatabase) {
        db.execSQL(
            "CREATE TABLE IF NOT EXISTS qrz_cache(" +
                "call TEXT PRIMARY KEY, fetched_at INTEGER NOT NULL, " +
                "name TEXT, qth TEXT, locator TEXT, country TEXT)"
        )
    }

    override fun onUpgrade(db: SQLiteDatabase, oldV: Int, newV: Int) { /* v1 only */ }

    /** Cached result for [call] if present and younger than [maxAgeDays], else null. */
    fun get(call: String, maxAgeDays: Int = 365, nowMs: Long = System.currentTimeMillis()): QrzResult? {
        if (maxAgeDays <= 0) return null
        val minFetched = nowMs - maxAgeDays * 86_400_000L
        readableDatabase.query(
            "qrz_cache",
            arrayOf("name", "qth", "locator", "country"),
            "call = ? AND fetched_at >= ?",
            arrayOf(call, minFetched.toString()),
            null, null, null,
        ).use { c ->
            if (!c.moveToFirst()) return null
            return QrzResult(
                call = call,
                name = c.getString(0) ?: "",
                qth = c.getString(1) ?: "",
                locator = c.getString(2) ?: "",
                country = c.getString(3) ?: "",
                error = "",
            )
        }
    }

    /** Insert or replace [r] with the current timestamp. No-ops on a blank call. */
    fun put(r: QrzResult, nowMs: Long = System.currentTimeMillis()) {
        if (r.call.isEmpty()) return
        val v = ContentValues().apply {
            put("call", r.call)
            put("fetched_at", nowMs)
            put("name", r.name)
            put("qth", r.qth)
            put("locator", r.locator)
            put("country", r.country)
        }
        writableDatabase.insertWithOnConflict("qrz_cache", null, v, SQLiteDatabase.CONFLICT_REPLACE)
    }

    private companion object {
        const val DB_NAME = "qrz-cache-mobile.sqlite"
    }
}
