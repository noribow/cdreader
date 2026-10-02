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

    @JvmStatic external fun nativeFormats(): String
    @JvmStatic external fun nativeOpen(fd: Int, interfaceNumber: Int, endpointIn: Int, endpointOut: Int): Long
    @JvmStatic external fun nativeClose(handle: Long)
    @JvmStatic external fun nativeCancel(handle: Long)
    @JvmStatic external fun nativeInquiry(handle: Long): String
    @JvmStatic external fun nativeIsReady(handle: Long): Boolean
    @JvmStatic external fun nativeReadToc(handle: Long): IntArray
    @JvmStatic external fun nativeLookupCddb(
        handle: Long,
        enabled: Boolean,
        server: String,
        matchIndex: Int,
        http: HttpGet?,
    ): Array<String>
    @JvmStatic external fun nativeAlbumFolderName(handle: Long): String
    @JvmStatic external fun nativeTrackFileName(handle: Long, track: Int, format: String): String
    @JvmStatic external fun nativeBeginRip(
        handle: Long,
        format: String,
        readOffset: Int,
        maxRetries: Int,
        verify: Boolean,
        useC2: Boolean,
    )
    @JvmStatic external fun nativeRipTrack(handle: Long, track: Int, path: String, listener: RipProgressListener?): IntArray
    @JvmStatic external fun nativeCheckAccurateRip(handle: Long, enabled: Boolean, http: HttpGet?): IntArray
    @JvmStatic external fun nativeAccurateRipError(handle: Long): String
    @JvmStatic external fun nativeC2Status(handle: Long): Int
    @JvmStatic external fun nativeRipLog(handle: Long): String
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
    val cddbHex: String get() = "%08X".format(cddbId)
}

/** CDDB lookup result; empty strings mean "unknown". */
data class DiscMetadata(
    val found: Boolean,
    /** "exact" / "inexact" when found, otherwise why not (e.g. "disabled"). */
    val message: String,
    val artist: String,
    val album: String,
    val year: String,
    val genre: String,
    /** Output folder: "Artist - Album" or "cd_<CDDB id>". */
    val folderName: String,
    val accurateRipId: String,
    val chosenMatch: Int,
    /** "category/discid  Artist / Album" for every match of the query. */
    val matches: List<String>,
    /** Per TOC entry (same order as [DiscToc.tracks]): "Title" or "Artist / Title", "" if unknown. */
    val trackLabels: List<String>,
)

data class RipResult(
    val sectors: Int,
    val unreadableSectors: Int,
    val retries: Int,
    val paddedSamples: Int,
    val crc32: Int,
    val accurateRipV1: Int,
    val accurateRipV2: Int,
    /** Read with C2 error pointers (#33). */
    val c2: Boolean,
    /** Sectors the drive flagged with C2 errors (each re-read on its own). */
    val c2ErrorSectors: Int,
    val c2Rereads: Int,
    /** Still C2 errors after the retries: the best read was kept. */
    val c2Unresolved: Int,
    /** Output sectors at suspicious positions (listed in rip.log). */
    val suspiciousSectors: Int,
) {
    val clean: Boolean get() = unreadableSectors == 0 && suspiciousSectors == 0
}

/** C2 error pointers of a rip (see [CdSession.c2Status]). */
enum class C2Status { DISABLED, NOT_SUPPORTED, USED, GIVEN_UP }

enum class AccurateRipStatus { FOUND, NOT_FOUND, ERROR, DISABLED }

data class AccurateRipTrack(
    val number: Int,
    /** Bit 0: our v1 checksum matched, bit 1: v2 matched. */
    val versions: Int,
    val v1Confidence: Int,
    val v2Confidence: Int,
    val totalConfidence: Int,
    val matchingPressings: Int,
    val pressings: Int,
) {
    val accurate: Boolean get() = versions != 0
    val inDatabase: Boolean get() = totalConfidence > 0
    val matchedVersion: String
        get() = when (versions) {
            1 -> "v1"
            2 -> "v2"
            3 -> "v1+v2"
            else -> ""
        }
}

data class AccurateRipSummary(
    val status: AccurateRipStatus,
    val pressings: Int,
    val accurateTracks: Int,
    val tracksInDatabase: Int,
    val tracks: List<AccurateRipTrack>,
    val error: String,
)

/** "mm:ss.ff" for a sector count (75 sectors per second), as the CLI prints it. */
fun formatMsf(sectors: Int): String =
    "%02d:%02d.%02d".format(sectors / (75 * 60), sectors / 75 % 60, sectors % 75)

/**
 * A CD drive opened over USB. All methods except [cancel] block and must be
 * called from one worker thread (the lookups use the network).
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

    /** Also forgets the metadata and rip results of the previous disc. */
    fun readToc(): DiscToc {
        val v = NativeCd.nativeReadToc(handle)
        val tracks = (4 until v.size step 4).map { i ->
            TrackInfo(v[i], v[i + 1], v[i + 2], v[i + 3] and 1 != 0, v[i + 3] and 2 != 0)
        }
        return DiscToc(v[0], v[1], v[2], v[3], tracks)
    }

    /**
     * Looks the disc up on CDDB ([server] "" = default) and uses the result
     * for file names and tags; disabled, it clears the metadata. Network
     * problems only end up in [DiscMetadata.message].
     */
    fun lookupCddb(enabled: Boolean, server: String, matchIndex: Int, http: HttpGet): DiscMetadata {
        val v = NativeCd.nativeLookupCddb(handle, enabled, server, matchIndex, http)
        val matchCount = v[9].toInt()
        val matches = v.slice(10 until 10 + matchCount)
        val labels = v.drop(10 + matchCount)
        return DiscMetadata(
            v[0] == "1", v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8].toInt(), matches, labels
        )
    }

    /** "Artist - Album" from CDDB, otherwise "cd_<CDDB id>". */
    fun albumFolderName(): String = NativeCd.nativeAlbumFolderName(handle)

    /** "NN - Title.flac" from CDDB, otherwise "TrackNN.flac". */
    fun trackFileName(track: Int, format: String): String = NativeCd.nativeTrackFileName(handle, track, format)

    /**
     * Starts a rip ([format]: a name from [NativeCd.nativeFormats]); forgets the results of the previous one.
     * [useC2]: read with C2 error pointers when the drive supports them.
     */
    fun beginRip(format: String, readOffset: Int, maxRetries: Int, verify: Boolean, useC2: Boolean) =
        NativeCd.nativeBeginRip(handle, format, readOffset, maxRetries, verify, useC2)

    /** Whether the current rip reads with C2 error pointers (known after [beginRip]). */
    fun c2Status(): C2Status = when (NativeCd.nativeC2Status(handle)) {
        1 -> C2Status.NOT_SUPPORTED
        2 -> C2Status.USED
        3 -> C2Status.GIVEN_UP
        else -> C2Status.DISABLED
    }

    /**
     * Rips a track to the local file [path] (seekable: headers are patched at
     * the end). Throws java.util.concurrent.CancellationException after
     * [cancel], IOException on errors.
     */
    fun ripTrack(track: Int, path: String, listener: RipProgressListener?): RipResult {
        val v = NativeCd.nativeRipTrack(handle, track, path, listener)
        return RipResult(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7] != 0, v[8], v[9], v[10], v[11])
    }

    /** Compares the tracks ripped since [beginRip] with the AccurateRip database. Never fails for network problems. */
    fun checkAccurateRip(enabled: Boolean, http: HttpGet): AccurateRipSummary {
        val v = NativeCd.nativeCheckAccurateRip(handle, enabled, http)
        val status = when (v[0]) {
            0 -> AccurateRipStatus.FOUND
            1 -> AccurateRipStatus.NOT_FOUND
            2 -> AccurateRipStatus.ERROR
            else -> AccurateRipStatus.DISABLED
        }
        val tracks = (4 until v.size step 7).map { i ->
            AccurateRipTrack(v[i], v[i + 1], v[i + 2], v[i + 3], v[i + 4], v[i + 5], v[i + 6])
        }
        return AccurateRipSummary(status, v[1], v[2], v[3], tracks, NativeCd.nativeAccurateRipError(handle))
    }

    /** rip.log text of the current rip (same layout as the Windows CLI). */
    fun ripLog(): String = NativeCd.nativeRipLog(handle)

    /** Thread-safe: makes a running [ripTrack] stop at the next block. */
    fun cancel() = NativeCd.nativeCancel(handle)

    override fun close() {
        if (!closed) {
            closed = true
            NativeCd.nativeClose(handle)
        }
    }
}
