package io.github.noribow.cdreader

import java.io.Closeable
import java.io.File

/** Called from the ripping thread with the progress in sectors. */
fun interface RipProgressListener {
    fun onProgress(doneSectors: Int, totalSectors: Int)
}

/**
 * Progress of [CdSession.ripImage] (#42): the part being read ([track] 0 is the
 * hidden track before track 1) and the sectors done of the whole image.
 */
fun interface ImageProgressListener {
    fun onProgress(track: Int, doneSectors: Int, totalSectors: Int)
}

/** Progress of the read offset detection (#37): track [step] of at most [steps], in sectors. */
fun interface OffsetProgressListener {
    fun onProgress(step: Int, steps: Int, track: Int, doneSectors: Int, totalSectors: Int)
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
    @JvmStatic external fun nativeDriveInfo(handle: Long): Array<String>
    @JvmStatic external fun nativeIsReady(handle: Long): Boolean
    @JvmStatic external fun nativeReadToc(handle: Long): IntArray
    @JvmStatic external fun nativeLookupCddb(
        handle: Long,
        enabled: Boolean,
        server: String,
        email: String,
        matchIndex: Int,
        http: HttpGet?,
    ): Array<String>
    @JvmStatic external fun nativeCheckCddbSettings(server: String, email: String): IntArray
    @JvmStatic external fun nativeTestCddb(server: String, email: String, http: HttpGet?): Array<String>
    @JvmStatic external fun nativeCddbServerPresets(): Array<String>
    @JvmStatic external fun nativeCddbServerSetting(value: String): Array<String>
    @JvmStatic external fun nativeAlbumFolderName(handle: Long): String
    @JvmStatic external fun nativeTrackFileName(handle: Long, track: Int, format: String): String
    @JvmStatic external fun nativeBeginRip(
        handle: Long,
        format: String,
        readOffset: Int,
        maxRetries: Int,
        verify: Boolean,
        useC2: Boolean,
        cacheMode: Int,
        offsetSource: Int,
        offsetDetail: String,
    )
    @JvmStatic external fun nativeRipTrack(handle: Long, track: Int, path: String, listener: RipProgressListener?): IntArray
    @JvmStatic external fun nativeImageFileNames(handle: Long, format: String, tracks: IntArray): Array<String>
    @JvmStatic external fun nativeRipImage(
        handle: Long,
        tracks: IntArray,
        imagePath: String,
        cuePath: String,
        listener: ImageProgressListener?,
    ): IntArray
    @JvmStatic external fun nativeDetectOffset(handle: Long, http: HttpGet?, listener: OffsetProgressListener?): Array<String>
    @JvmStatic external fun nativeSetDriveOffsetDbCache(handle: Long, path: String)
    @JvmStatic external fun nativeLookupDriveOffsetDb(handle: Long, http: HttpGet?): Array<String>
    @JvmStatic external fun nativeCheckAccurateRip(handle: Long, enabled: Boolean, http: HttpGet?): IntArray
    @JvmStatic external fun nativeAccurateRipError(handle: Long): String
    @JvmStatic external fun nativeC2Status(handle: Long): Int
    @JvmStatic external fun nativeCacheStatus(handle: Long): IntArray
    @JvmStatic external fun nativeRipLog(handle: Long): String
}

/** INQUIRY strings; [offsetKey] identifies the drive model for the saved read offsets (#37). */
data class DriveInfo(
    val vendor: String,
    val product: String,
    val revision: String,
    val displayName: String,
    val offsetKey: String,
)

/**
 * Where the read offset of a rip came from (rip.log); [code] is what nativeBeginRip expects.
 * SELECTED: chosen by the user from the candidates of an ambiguous detection.
 */
enum class OffsetSource(val code: Int) { MANUAL(0), SAVED(1), DETECTED(2), SELECTED(3) }

/** State of the AccurateRip drive offset database lookup (#37), in the order of the native codes. */
enum class DriveDbStatus { NOT_CHECKED, FOUND, NOT_LISTED, UNAVAILABLE }

/** The drive's entry in the AccurateRip drive offset database (DriveOffsets.bin). */
data class DriveOffsetDb(
    val status: DriveDbStatus,
    /** FOUND: the offset listed for the drive model. */
    val offset: Int,
    /** -1: unknown. */
    val submissions: Int,
    /** -1: unknown. */
    val agreePercent: Int,
    /** The drive's name in the database. */
    val name: String,
    /** "AccurateRip drive database: +6 (1234 submissions)" (rip.log). */
    val logLine: String,
)

/** One offset of pressings shifted against each other, for choosing by hand (#37). */
data class OffsetCandidateInfo(
    val offset: Int,
    /** Tracks read that match at it. */
    val tracks: Int,
    /** Submissions behind it, summed over the tracks. */
    val confidence: Int,
    /** 1-based AccurateRip records (pressings) that match at it. */
    val pressings: List<Int>,
)

/** Result of [CdSession.detectOffset], in the order of the native status codes. */
enum class OffsetDetectStatus {
    DETECTED, NOT_IN_DATABASE, LOOKUP_FAILED, NO_USABLE_TRACKS, NO_MATCH, NOT_ENOUGH, CONFLICT, CANCELLED,
    /** Every track matches the same offsets (pressings shifted against each other), none clearly best. */
    AMBIGUOUS,
}

/** An offset matched by another pressing, with its submissions summed over the tracks read. */
data class OffsetAlternative(val offset: Int, val confidence: Int)

data class OffsetDetection(
    val status: OffsetDetectStatus,
    /** DETECTED: the offset; otherwise the best candidate (if any track matched). */
    val offset: Int,
    val agreeingTracks: Int,
    val testedTracks: Int,
    val confidence: Int,
    /** "v1", "v2" or "v1+v2". */
    val version: String,
    /** Confirmed by the disc's only track in the database. */
    val singleTrack: Boolean,
    val usableTracks: Int,
    /** CONFLICT / AMBIGUOUS: the competing offsets. */
    val candidates: List<Int>,
    /** English one-line summary (as in rip.log). */
    val summary: String,
    /** "2 of 2 tracks agreed, v2" (rip.log, the saved note). */
    val agreement: String,
    val error: String,
    /** DETECTED / AMBIGUOUS with shifted pressings: the other pressings' offsets, highest confidence first. */
    val alternatives: List<OffsetAlternative> = emptyList(),
    /** The AccurateRip drive offset database for this drive (#37). */
    val driveDbStatus: DriveDbStatus = DriveDbStatus.NOT_CHECKED,
    val driveDbOffset: Int = 0,
    val driveDbSubmissions: Int = -1,
    /** DETECTED only because the drive database lists [offset]. */
    val decidedByDriveDb: Boolean = false,
    /** DETECTED at the drive database's offset. */
    val matchesDriveDb: Boolean = false,
    /** Shifted pressings: every candidate, [offset]'s first (empty otherwise). */
    val allCandidates: List<OffsetCandidateInfo> = emptyList(),
    /** "AccurateRip drive database: ..." */
    val driveDbLogLine: String = "",
    /** Note to save with a candidate the user chose ("selected by user from candidates +6, -145, -658"). */
    val selectionNote: String = "",
    /** "+6, -145, -658": the detail of [OffsetSource.SELECTED] for rip.log. */
    val candidateList: String = "",
)

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
    /** Not found because the server refused the greeting (gnudb: no contact e-mail address set, #38). */
    val needsContactEmail: Boolean = false,
)

/** Problems of the CDDB settings as typed (cdr::CddbConfigProblem). */
enum class CddbSettingProblem { NONE, SERVER_SCHEME, SERVER_HOST, SERVER_CHARACTERS, EMAIL_SHAPE, HELLO_CHARACTERS }

data class CddbSettingsCheck(val server: CddbSettingProblem, val email: CddbSettingProblem)

/** Result of [CddbSettings.test]. */
data class CddbTestResult(
    /** A 2xx answer of the server. */
    val ok: Boolean,
    /** CDDB response code, 0 if none (transport or HTTP error). */
    val code: Int,
    /** The server's status line, or the error (English). */
    val message: String,
    /** e.g. "4120000 database entries", may be empty. */
    val detail: String,
    /** The server refused the greeting: a contact e-mail address is needed. */
    val needsContactEmail: Boolean,
    /** Works, but the server is gnudb and no contact address is set (lookups may be refused). */
    val missingEmail: Boolean,
)

/** A CDDB server the user can pick by name (#46, cdr::CddbServerPreset). */
data class CddbServerPreset(val id: String, val label: String, val url: String)

/** A saved server setting (#46): what to store, and which dropdown entry it is. */
data class CddbServerSetting(
    /** Preset id ("gnudb", "japdb"), a custom URL, or "" for the default server. */
    val value: String,
    /** A preset id, or [CddbSettings.CUSTOM] for any other URL. */
    val choice: String,
)

/**
 * CDDB server and contact e-mail address (#38), as stored in the app's
 * settings: the server is a preset id ("gnudb", "japdb", #46) or a custom
 * URL; "" means the default server / the anonymous greeting.
 */
object CddbSettings {
    /** The dropdown entry for a server URL typed by the user (cdr::kCddbServerCustom). */
    const val CUSTOM = "custom"

    /** The presets, the default (gnudb) first. */
    val presets: List<CddbServerPreset> by lazy {
        NativeCd.nativeCddbServerPresets().toList().chunked(3).filter { it.size == 3 }
            .map { CddbServerPreset(it[0], it[1], it[2]) }
    }

    /**
     * The value to store and the dropdown entry for a saved server setting. A URL
     * saved before #46 that names a preset becomes that preset (migration).
     */
    fun server(saved: String): CddbServerSetting {
        val v = NativeCd.nativeCddbServerSetting(saved)
        return CddbServerSetting(v[0], v[1])
    }

    /** Validation only, no I/O: may be called on the UI thread. */
    fun check(server: String, email: String): CddbSettingsCheck {
        val v = NativeCd.nativeCheckCddbSettings(server, email)
        val values = CddbSettingProblem.entries
        return CddbSettingsCheck(
            values.getOrElse(v[0]) { CddbSettingProblem.SERVER_HOST },
            values.getOrElse(v[1]) { CddbSettingProblem.EMAIL_SHAPE },
        )
    }

    /** Connection test: one "stat" request with the lookups' greeting. Network: never on the UI thread. */
    fun test(server: String, email: String, http: HttpGet): CddbTestResult {
        val v = NativeCd.nativeTestCddb(server, email, http)
        return CddbTestResult(v[0] == "1", v[1].toIntOrNull() ?: 0, v[2], v[3], v[4] == "1", v[5] == "1")
    }
}

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
    /** FUA commands or cache flushes sent before re-reads (#34). */
    val cacheDefeats: Int,
) {
    val clean: Boolean get() = unreadableSectors == 0 && suspiciousSectors == 0
}

/** File names of a single-file rip (#42), see [CdSession.imageFileNames]. */
data class ImageNames(
    /** "Artist - Album.flac", or "CDImage.flac" without CDDB data. */
    val imageFile: String,
    /** "Artist - Album.cue" / "CDImage.cue". */
    val cueFile: String,
    /** The format carries the CUE sheet inside the file (FLAC, Ogg FLAC: CUESHEET; MKA: chapters). */
    val embeddedCue: Boolean,
    /** The image starts with the hidden track before track 1 (HTOA). */
    val withHtoa: Boolean,
)

/** Result of [CdSession.ripImage]. */
data class ImageResult(
    /** CRC32 of all the audio in the image. */
    val crc32: Int,
    val embeddedCue: Boolean,
    val withHtoa: Boolean,
    /** Per part in image order: track number (0: the HTOA) and its result. */
    val parts: List<Pair<Int, RipResult>>,
)

/** C2 error pointers of a rip (see [CdSession.c2Status]). */
enum class C2Status { DISABLED, NOT_SUPPORTED, USED, GIVEN_UP }

/**
 * Drive cache defeat before re-reads (#34), in the order of the 詳細設定
 * spinner (R.array.cache_modes); [code] is what nativeBeginRip expects.
 */
enum class CacheMode(val code: Int, val key: String) {
    AUTO(0, "auto"), FUA(1, "fua"), FLUSH(2, "flush"), NONE(3, "none");

    companion object {
        fun fromKey(key: String?): CacheMode = CacheMode.entries.firstOrNull { it.key == key } ?: AUTO
    }
}

/** Result of the cache test of [CacheMode.AUTO] (NOT_TESTED for the other modes). */
enum class CacheResult { NOT_TESTED, NO_CACHE, FUA_WORKS, FUA_IGNORED, FUA_REJECTED, UNKNOWN }

/** What a rip does before re-reads. */
enum class CacheMethod { NONE, FUA, FLUSH }

data class CacheStatus(
    val result: CacheResult,
    val method: CacheMethod,
    /** Buffer size reported by the drive, 0 if unknown. */
    val cacheKb: Int,
    /** The drive rejected FUA during the rip: flushes from then on. */
    val fuaGivenUp: Boolean,
)

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

    fun driveInfo(): DriveInfo {
        val v = NativeCd.nativeDriveInfo(handle)
        return DriveInfo(v[0], v[1], v[2], v[3], v[4])
    }

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
     * Looks the disc up on CDDB ([server] "" = default, [email] "" = the
     * anonymous greeting, otherwise the contact address user@host sent to the
     * server) and uses the result for file names and tags; disabled, it
     * clears the metadata. Network problems only end up in [DiscMetadata.message].
     */
    fun lookupCddb(enabled: Boolean, server: String, email: String, matchIndex: Int, http: HttpGet): DiscMetadata {
        val v = NativeCd.nativeLookupCddb(handle, enabled, server, email, matchIndex, http)
        val matchCount = v[9].toInt()
        val matches = v.slice(11 until 11 + matchCount)
        val labels = v.drop(11 + matchCount)
        return DiscMetadata(
            v[0] == "1", v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8].toInt(), matches, labels, v[10] == "1"
        )
    }

    /** "Artist - Album" from CDDB, otherwise "cd_<CDDB id>". */
    fun albumFolderName(): String = NativeCd.nativeAlbumFolderName(handle)

    /** "NN - Title.flac" from CDDB, otherwise "TrackNN.flac". */
    fun trackFileName(track: Int, format: String): String = NativeCd.nativeTrackFileName(handle, track, format)

    /**
     * Starts a rip ([format]: a name from [NativeCd.nativeFormats]); forgets the results of the previous one.
     * [useC2]: read with C2 error pointers when the drive supports them.
     * [cacheMode]: drive cache defeat; [CacheMode.AUTO] tests the drive once per disc (a few seconds).
     * [offsetSource] / [offsetDetail]: where [readOffset] came from, for rip.log (#37).
     */
    fun beginRip(
        format: String,
        readOffset: Int,
        maxRetries: Int,
        verify: Boolean,
        useC2: Boolean,
        cacheMode: CacheMode,
        offsetSource: OffsetSource,
        offsetDetail: String,
    ) = NativeCd.nativeBeginRip(
        handle, format, readOffset, maxRetries, verify, useC2, cacheMode.code, offsetSource.code, offsetDetail
    )

    /**
     * Detects the read offset with AccurateRip (#37): reads up to 3 tracks of
     * the disc; the offset is confirmed when 2 of them agree. Network and read
     * problems end up in the status; [cancel] makes it return CANCELLED.
     */
    fun detectOffset(http: HttpGet, listener: OffsetProgressListener?): OffsetDetection {
        val v = NativeCd.nativeDetectOffset(handle, http, listener)
        return OffsetDetection(
            OffsetDetectStatus.entries.getOrElse(v[0].toInt()) { OffsetDetectStatus.NO_MATCH },
            v[1].toInt(), v[2].toInt(), v[3].toInt(), v[4].toInt(), v[5], v[6] == "1", v[7].toInt(),
            if (v[8].isEmpty()) emptyList() else v[8].split(",").map { it.toInt() },
            v[9], v[10], v[11],
            v.getOrNull(12).orEmpty().split(",").filter { it.isNotEmpty() }.map {
                val (offset, confidence) = it.split(":")
                OffsetAlternative(offset.toInt(), confidence.toInt())
            },
            DriveDbStatus.entries.getOrElse(v.getOrNull(13)?.toIntOrNull() ?: 0) { DriveDbStatus.NOT_CHECKED },
            v.getOrNull(14)?.toIntOrNull() ?: 0,
            v.getOrNull(15)?.toIntOrNull() ?: -1,
            v.getOrNull(16) == "1",
            v.getOrNull(17) == "1",
            v.getOrNull(18).orEmpty().split(";").filter { it.isNotEmpty() }.map {
                val f = it.split(":")
                OffsetCandidateInfo(
                    f[0].toInt(), f[1].toInt(), f[2].toInt(),
                    f.getOrNull(3).orEmpty().split("+").filter { p -> p.isNotEmpty() }.map { p -> p.toInt() },
                )
            },
            v.getOrNull(19).orEmpty(),
            v.getOrNull(20).orEmpty(),
            v.getOrNull(21).orEmpty(),
        )
    }

    /** Where DriveOffsets.bin (AccurateRip drive offset database, #37) is kept between runs. */
    fun setDriveOffsetDbCache(file: File) = NativeCd.nativeSetDriveOffsetDbCache(handle, file.path)

    /**
     * The drive's entry in the AccurateRip drive offset database (#37): the
     * stored file when fresh (30 days), otherwise downloaded with [http]
     * (null: only the stored file). Network problems end up in the status.
     * Not on the UI thread.
     */
    fun lookupDriveOffsetDb(http: HttpGet?): DriveOffsetDb {
        val v = NativeCd.nativeLookupDriveOffsetDb(handle, http)
        return DriveOffsetDb(
            DriveDbStatus.entries.getOrElse(v[0].toInt()) { DriveDbStatus.NOT_CHECKED },
            v[1].toInt(), v[2].toInt(), v[3].toInt(), v[4], v[5],
        )
    }

    /** The drive cache check of the current rip (known after [beginRip]). */
    fun cacheStatus(): CacheStatus {
        val v = NativeCd.nativeCacheStatus(handle)
        return CacheStatus(
            CacheResult.entries.getOrElse(v[0]) { CacheResult.UNKNOWN },
            CacheMethod.entries.getOrElse(v[1]) { CacheMethod.NONE },
            v[2],
            v[3] != 0,
        )
    }

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
        return RipResult(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7] != 0, v[8], v[9], v[10], v[11], v[12])
    }

    /**
     * Single-file mode (#42): the names of the image of [tracks] (track
     * numbers; empty: every audio track) in [format], from the CDDB data.
     * Throws IllegalArgumentException when the tracks are not consecutive
     * audio tracks (the file would be no image of the disc).
     */
    fun imageFileNames(format: String, tracks: List<Int>): ImageNames {
        val v = NativeCd.nativeImageFileNames(handle, format, tracks.toIntArray())
        return ImageNames(v[0], v[1], v[2] == "1", v[3] == "1")
    }

    /**
     * Rips [tracks] (as [imageFileNames], in the format of [beginRip]) into
     * one local file [imagePath] (seekable, as [ripTrack]), the HTOA first
     * when the disc has one and track 1 is included, with the CUE sheet
     * embedded where the format can carry it, and writes the CUE sheet to
     * [cuePath]. AccurateRip checksums are computed per track, so
     * [checkAccurateRip] and [ripLog] work as after [ripTrack]. Throws
     * java.util.concurrent.CancellationException after [cancel],
     * IllegalArgumentException for a selection that is no image and
     * IOException on errors; both files are deleted then.
     */
    fun ripImage(tracks: List<Int>, imagePath: String, cuePath: String, listener: ImageProgressListener?): ImageResult {
        val v = NativeCd.nativeRipImage(handle, tracks.toIntArray(), imagePath, cuePath, listener)
        val parts = (0 until v[2]).map { k ->
            val i = 3 + k * 14
            v[i] to RipResult(
                v[i + 1], v[i + 2], v[i + 3], v[i + 4], v[i + 5], v[i + 6], v[i + 7], v[i + 8] != 0, v[i + 9],
                v[i + 10], v[i + 11], v[i + 12], v[i + 13]
            )
        }
        return ImageResult(v[0], v[1] and 1 != 0, v[1] and 2 != 0, parts)
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

    /** Thread-safe: makes a running [ripTrack], [ripImage] or [detectOffset] stop at the next block. */
    fun cancel() = NativeCd.nativeCancel(handle)

    override fun close() {
        if (!closed) {
            closed = true
            NativeCd.nativeClose(handle)
        }
    }
}
