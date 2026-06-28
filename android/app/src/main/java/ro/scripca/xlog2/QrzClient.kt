package ro.scripca.xlog2

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import java.net.URLEncoder

/**
 * Direct qrz.com XML-API client, a Kotlin port of the desktop
 * `src/core/services/Qrz.cpp`. The mobile native core is built without libcurl
 * (`XLOG_MOBILE_QRZ=OFF`), so the qrz.com tier lives here instead.
 *
 * Holds a session key in memory (login → key → callsign queries reuse it); a
 * stale key triggers one re-login + retry, exactly as the desktop does.
 * Stateless otherwise — caching is handled by the caller ([XlogRepository]).
 */
class QrzClient(private val http: OkHttpClient = OkHttpClient()) {

    @Volatile private var sessionKey: String = ""

    /**
     * Resolve [callsign] against qrz.com using [user]/[password]. Always returns
     * a [QrzResult]; a non-empty [QrzResult.error] means "no record" or a
     * login/network failure (the call still echoes back in `call`).
     */
    suspend fun lookup(callsign: String, user: String, password: String): QrzResult =
        withContext(Dispatchers.IO) {
            try {
                if (sessionKey.isEmpty()) {
                    val err = login(user, password)
                    if (err != null) return@withContext QrzResult(callsign, error = err)
                }

                var body = query(callsign)
                var err = tag(body, "Error")

                // A stale key needs a fresh login and one retry (Qrz.cpp:226-234).
                if (err.isNotEmpty() &&
                    (err.contains("ession") || err.contains("nvalid"))
                ) {
                    sessionKey = ""
                    val loginErr = login(user, password)
                    if (loginErr != null) return@withContext QrzResult(callsign, error = loginErr)
                    body = query(callsign)
                    err = tag(body, "Error")
                }

                if (err.isNotEmpty()) return@withContext QrzResult(callsign, error = err)

                val call = tag(body, "call")
                if (call.isEmpty())
                    return@withContext QrzResult(callsign, error = "No QRZ record for $callsign")

                // fname + name → "First Last" (Qrz.cpp:250-252).
                val fn = tag(body, "fname")
                val ln = tag(body, "name")
                val name = when {
                    fn.isEmpty() -> ln
                    ln.isEmpty() -> fn
                    else -> "$fn $ln"
                }
                // addr2 (city) + state → "City, State" (Qrz.cpp:253-255).
                val city = tag(body, "addr2")
                val state = tag(body, "state")
                val qth = when {
                    city.isEmpty() -> state
                    state.isEmpty() -> city
                    else -> "$city, $state"
                }
                QrzResult(
                    call = call,
                    name = name,
                    qth = qth,
                    locator = tag(body, "grid"),
                    country = tag(body, "country"),
                    error = "",
                )
            } catch (e: Exception) {
                QrzResult(callsign, error = e.message ?: "QRZ lookup failed")
            }
        }

    /** Exchange credentials for a session key; returns an error string or null. */
    private fun login(user: String, password: String): String? {
        val url = "$BASE?username=${enc(user)}&password=${enc(password)}" +
            "&agent=xlog2-android-${BuildConfig.VERSION_NAME}"
        val body = httpGet(url)
        val key = tag(body, "Key")
        if (key.isEmpty()) {
            val err = tag(body, "Error")
            return err.ifEmpty { "QRZ login failed" }
        }
        sessionKey = key
        return null
    }

    private fun query(callsign: String): String =
        httpGet("$BASE?s=${enc(sessionKey)}&callsign=${enc(callsign)}")

    private fun httpGet(url: String): String {
        val req = Request.Builder().url(url).build()
        http.newCall(req).execute().use { resp ->
            return resp.body?.string() ?: ""
        }
    }

    private companion object {
        const val BASE = "https://xmldata.qrz.com/xml/current/"

        fun enc(s: String): String = URLEncoder.encode(s, "UTF-8")

        /** Value of the first <tag>…</tag>, unescaped, or "" (mirrors xmlTag). */
        fun tag(xml: String, tag: String): String {
            val open = "<$tag>"
            val close = "</$tag>"
            val a = xml.indexOf(open)
            if (a < 0) return ""
            val start = a + open.length
            val b = xml.indexOf(close, start)
            if (b < 0) return ""
            return unescape(xml.substring(start, b))
        }

        fun unescape(s: String): String = s
            .replace("&lt;", "<")
            .replace("&gt;", ">")
            .replace("&quot;", "\"")
            .replace("&apos;", "'")
            .replace("&#39;", "'")
            .replace("&amp;", "&")  // last, so it doesn't re-trigger the others
    }
}
