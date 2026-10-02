package io.github.noribow.cdreader

import java.io.ByteArrayOutputStream
import java.io.IOException
import java.io.InputStream
import java.net.HttpURLConnection
import java.net.URI

/**
 * Outcome of [HttpGet.get]. The native code reads these fields directly
 * (platform/android/jni_bridge.cpp, JavaHttpClient): `error` is set when the
 * request did not complete, otherwise `status` and `body` hold the response
 * (any HTTP status, e.g. 404 for a disc unknown to AccurateRip).
 */
class HttpResult(
    @JvmField val status: Int,
    @JvmField val body: ByteArray?,
    @JvmField val error: String?,
)

/**
 * HTTP(S) GET for the online lookups of the native code (CDDB, AccurateRip).
 * Called from native code on the worker thread, never on the UI thread
 * (Android forbids network access there). Never throws.
 */
class HttpGet(
    private val userAgent: String,
    private val connectTimeoutMs: Int = 10_000,
    private val readTimeoutMs: Int = 20_000,
    private val maxBodyBytes: Int = 8 shl 20,
) {
    fun get(url: String): HttpResult {
        var connection: HttpURLConnection? = null
        return try {
            val c = URI(url).toURL().openConnection() as HttpURLConnection
            connection = c
            c.connectTimeout = connectTimeoutMs
            c.readTimeout = readTimeoutMs
            c.instanceFollowRedirects = true
            c.useCaches = false
            c.setRequestProperty("User-Agent", userAgent)
            val status = c.responseCode
            val stream = if (status >= 400) c.errorStream else c.inputStream
            val body = stream?.use { readLimited(it) } ?: ByteArray(0)
            HttpResult(status, body, null)
        } catch (e: Exception) {
            HttpResult(0, null, e.message ?: e.javaClass.simpleName)
        } finally {
            connection?.disconnect()
        }
    }

    private fun readLimited(input: InputStream): ByteArray {
        val out = ByteArrayOutputStream()
        val buffer = ByteArray(1 shl 14)
        while (true) {
            val n = input.read(buffer)
            if (n < 0) break
            if (out.size() + n > maxBodyBytes) throw IOException("response too large")
            out.write(buffer, 0, n)
        }
        return out.toByteArray()
    }
}
