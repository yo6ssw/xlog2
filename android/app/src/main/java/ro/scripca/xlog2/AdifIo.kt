package ro.scripca.xlog2

import android.content.Context
import android.net.Uri
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * ADIF import/export glue between Android's Storage Access Framework (content
 * Uris from the file picker) and the native core's reused Adif.cpp. The parsing
 * itself happens in C++ — this only moves bytes across the content resolver.
 */
object AdifIo {

    suspend fun import(context: Context, uri: Uri): Int = withContext(Dispatchers.IO) {
        val text = context.contentResolver.openInputStream(uri)?.use {
            it.readBytes().toString(Charsets.UTF_8)
        } ?: return@withContext 0
        XlogRepository.get(context).importAdif(text)
    }

    suspend fun export(context: Context, uri: Uri): Boolean = withContext(Dispatchers.IO) {
        val adif = XlogRepository.get(context).exportAdif()
        context.contentResolver.openOutputStream(uri)?.use {
            it.write(adif.toByteArray(Charsets.UTF_8))
        } ?: return@withContext false
        true
    }
}
