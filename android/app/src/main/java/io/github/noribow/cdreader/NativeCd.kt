package io.github.noribow.cdreader

import java.io.Closeable

/** Called from the ripping thread with the progress in sectors. */
fun interface RipProgressListener {
    fun onProgress(doneSectors: Int, totalSectors: Int)
}

/** JNI entry points of libcdreader_jni (platform/android/jni_bridge.cpp). */
object NativeCd {
    init {
        System.loadLibrary("cdreader_jni")
    }

    @JvmStatic external fun nativeOpen(fd: Int, interfaceNumber: Int, endpointIn: Int, endpointOut: Int): Long
    @JvmStatic external fun nativeClose(handle: Long)
    @JvmStatic external fun nativeCancel(handle: Long)
    @JvmStatic external fun nativeInquiry(handle: Long): String
    @JvmStatic external fun nativeIsReady(handle: Long): Boolean
    @JvmStatic external fun nativeReadToc(handle: Long): IntArray
    @JvmStatic external fun nativeRipTrack(
        handle: Long,
        track: Int,
        path: String,
        readOffset: Int,
        maxRetries: Int,
        verify: Boolean,
        listener: RipProgressListener?,
    ): IntArray
}

data class TrackInfo(
    val number: Int,
    val startLba: Int,
    val lengthSectors: Int,
    val isAudio: Boolean,
    val preEmphasis: Boolean,
)

data class DiscToc(
    val firstTrack: Int,
    val lastTrack: Int,
    val leadOutLba: Int,
    val cddbId: Int,
    val tracks: List<TrackInfo>,
) {
    val cddbHex: String get() = "%08x".format(cddbId)
}

data class RipResult(
    val sectors: Int,
    val unreadableSectors: Int,
    val retries: Int,
    val paddedSamples: Int,
    val crc32: Int,
) {
    val clean: Boolean get() = unreadableSectors == 0
}

/** "mm:ss.ff" for a sector count (75 sectors per second), as the CLI prints it. */
fun formatMsf(sectors: Int): String =
    "%02d:%02d.%02d".format(sectors / (75 * 60), sectors / 75 % 60, sectors % 75)

/**
 * A CD drive opened over USB. All methods except [cancel] block and must be
 * called from one worker thread.
 */
class CdSession private constructor(private val handle: Long) : Closeable {
    companion object {
        /** `fd` is UsbDeviceConnection.getFileDescriptor(); the connection must stay open. */
        fun open(fd: Int, interfaceNumber: Int, endpointIn: Int, endpointOut: Int): CdSession =
            CdSession(NativeCd.nativeOpen(fd, interfaceNumber, endpointIn, endpointOut))
    }

    private var closed = false

    fun inquiry(): String = NativeCd.nativeInquiry(handle)

    fun isReady(): Boolean = NativeCd.nativeIsReady(handle)

    fun readToc(): DiscToc {
        val v = NativeCd.nativeReadToc(handle)
        val tracks = (4 until v.size step 4).map { i ->
            TrackInfo(v[i], v[i + 1], v[i + 2], v[i + 3] and 1 != 0, v[i + 3] and 2 != 0)
        }
        return DiscToc(v[0], v[1], v[2], v[3], tracks)
    }

    /** Throws java.util.concurrent.CancellationException after [cancel], IOException on errors. */
    fun ripTrack(
        track: Int,
        path: String,
        readOffset: Int,
        maxRetries: Int,
        verify: Boolean,
        listener: RipProgressListener?,
    ): RipResult {
        val v = NativeCd.nativeRipTrack(handle, track, path, readOffset, maxRetries, verify, listener)
        return RipResult(v[0], v[1], v[2], v[3], v[4])
    }

    /** Thread-safe: makes a running [ripTrack] stop at the next block. */
    fun cancel() = NativeCd.nativeCancel(handle)

    override fun close() {
        if (!closed) {
            closed = true
            NativeCd.nativeClose(handle)
        }
    }
}
