package io.github.noribow.cdreader

import android.app.Activity
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.DocumentsContract
import android.text.method.ScrollingMovementMethod
import android.view.View
import android.view.WindowInsets
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.ListView
import android.widget.ProgressBar
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.Spinner
import android.widget.TextView
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.IOException
import java.util.concurrent.CancellationException
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * Single screen: connect to a USB CD drive, show its TOC (with titles from
 * CDDB), rip the selected tracks to FLAC, WAV, Opus or Vorbis files in a folder chosen with
 * the Storage Access Framework and check them against AccurateRip.
 * USB I/O, network lookups and ripping run on one worker thread ([worker]);
 * the native session is only touched from there (except cancel()).
 */
class MainActivity : Activity() {
    private companion object {
        const val ACTION_USB_PERMISSION = "io.github.noribow.cdreader.USB_PERMISSION"
        const val REQUEST_FOLDER = 1
        const val PREFS = "settings"
        const val PREF_FOLDER = "outputTree"
        const val PREF_OFFSET = "readOffset"
        const val PREF_FORMAT = "format"
        const val PREF_CDDB = "cddb"
        const val PREF_ACCURATERIP = "accurateRip"
        const val MAX_RETRIES = 5
        const val READY_WAIT_SECONDS = 30
        const val USER_AGENT = "cdreader/0.1.0 (Android)"
        const val CDDB_SERVER = ""  // the default server (gnudb.org)
    }

    private lateinit var usbManager: UsbManager
    private val worker: ExecutorService = Executors.newSingleThreadExecutor()
    private val http = HttpGet(USER_AGENT)

    private lateinit var buttonConnect: Button
    private lateinit var buttonFolder: Button
    private lateinit var buttonRip: Button
    private lateinit var textDrive: TextView
    private lateinit var textDisc: TextView
    private lateinit var textFolder: TextView
    private lateinit var textStatus: TextView
    private lateinit var textResults: TextView
    private lateinit var listTracks: ListView
    private lateinit var spinnerMatch: Spinner
    private lateinit var editOffset: EditText
    private lateinit var checkVerify: CheckBox
    private lateinit var checkCddb: CheckBox
    private lateinit var checkAccurateRip: CheckBox
    private lateinit var groupFormat: RadioGroup
    private lateinit var radioFlac: RadioButton
    private lateinit var radioWav: RadioButton
    private lateinit var radioOpus: RadioButton
    private lateinit var radioVorbis: RadioButton
    private lateinit var radioMka: RadioButton
    private lateinit var progress: ProgressBar

    // Owned by the worker thread once opened.
    private var device: UsbDevice? = null
    private var connection: UsbDeviceConnection? = null
    private var massStorage: MassStorageInterface? = null
    @Volatile private var session: CdSession? = null
    private val sessionLock = Any()  // cancel() from the UI thread vs. close() on the worker

    // Read by the worker when a disc is loaded.
    @Volatile private var cddbEnabled = true
    // Set on the UI thread; stops a rip between tracks, even before it reached native code.
    @Volatile private var cancelRequested = false

    // UI thread state.
    private var driveName = ""
    private var toc: DiscToc? = null
    private var metadata: DiscMetadata? = null
    private var outputTree: Uri? = null
    private var ripping = false

    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val dev = intent.usbDevice() ?: return
            when (intent.action) {
                ACTION_USB_PERMISSION ->
                    if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) openDevice(dev)
                    else setStatus(getString(R.string.permission_denied))
                UsbManager.ACTION_USB_DEVICE_DETACHED ->
                    if (dev.deviceName == device?.deviceName) {
                        cancelRip()
                        closeDevice()
                        setStatus(getString(R.string.disconnected))
                    }
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        usbManager = getSystemService(Context.USB_SERVICE) as UsbManager

        buttonConnect = findViewById(R.id.buttonConnect)
        buttonFolder = findViewById(R.id.buttonFolder)
        buttonRip = findViewById(R.id.buttonRip)
        textDrive = findViewById(R.id.textDrive)
        textDisc = findViewById(R.id.textDisc)
        textFolder = findViewById(R.id.textFolder)
        textStatus = findViewById(R.id.textStatus)
        textResults = findViewById(R.id.textResults)
        listTracks = findViewById(R.id.listTracks)
        spinnerMatch = findViewById(R.id.spinnerMatch)
        editOffset = findViewById(R.id.editOffset)
        checkVerify = findViewById(R.id.checkVerify)
        checkCddb = findViewById(R.id.checkCddb)
        checkAccurateRip = findViewById(R.id.checkAccurateRip)
        groupFormat = findViewById(R.id.groupFormat)
        radioFlac = findViewById(R.id.radioFlac)
        radioWav = findViewById(R.id.radioWav)
        radioOpus = findViewById(R.id.radioOpus)
        radioVorbis = findViewById(R.id.radioVorbis)
        radioMka = findViewById(R.id.radioMka)
        progress = findViewById(R.id.progress)
        textResults.movementMethod = ScrollingMovementMethod()
        applySystemBarInsets(findViewById(R.id.root))

        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        editOffset.setText(prefs.getInt(PREF_OFFSET, 0).toString())
        prefs.getString(PREF_FOLDER, null)?.let { setOutputTree(Uri.parse(it)) }
        // Opus / Vorbis are only offered when the native library was built with them.
        val available = NativeCd.nativeFormats().split(",")
        for ((format, radio) in formatButtons()) radio.visibility = if (format in available) View.VISIBLE else View.GONE
        val saved = prefs.getString(PREF_FORMAT, "flac")
        (formatButtons().firstOrNull { it.first == saved && it.first in available }?.second ?: radioFlac).isChecked = true
        cddbEnabled = prefs.getBoolean(PREF_CDDB, true)
        checkCddb.isChecked = cddbEnabled
        checkAccurateRip.isChecked = prefs.getBoolean(PREF_ACCURATERIP, true)

        buttonConnect.setOnClickListener { if (connection == null) connect() else reloadDisc() }
        buttonFolder.setOnClickListener {
            startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQUEST_FOLDER)
        }
        buttonRip.setOnClickListener { if (ripping) cancelRip() else startRip() }
        groupFormat.setOnCheckedChangeListener { _, _ ->
            prefs.edit().putString(PREF_FORMAT, selectedFormat()).apply()
        }
        checkAccurateRip.setOnCheckedChangeListener { _, checked ->
            prefs.edit().putBoolean(PREF_ACCURATERIP, checked).apply()
        }
        checkCddb.setOnCheckedChangeListener { _, checked ->
            prefs.edit().putBoolean(PREF_CDDB, checked).apply()
            cddbEnabled = checked
            if (toc != null && !ripping) lookUpAgain(0)
        }
        spinnerMatch.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                // Also called for the selection made in showMetadata().
                val meta = metadata ?: return
                if (position != meta.chosenMatch && toc != null && !ripping) lookUpAgain(position)
            }

            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }

        val filter = IntentFilter().apply {
            addAction(ACTION_USB_PERMISSION)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        }
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(usbReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        else registerReceiver(usbReceiver, filter)

        val attached = intent.takeIf { it.action == UsbManager.ACTION_USB_DEVICE_ATTACHED }?.usbDevice()
        if (attached != null) requestAndOpen(attached) else connect()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        if (intent.action == UsbManager.ACTION_USB_DEVICE_ATTACHED && session == null) {
            intent.usbDevice()?.let { requestAndOpen(it) }
        }
    }

    override fun onDestroy() {
        unregisterReceiver(usbReceiver)
        cancelRip()
        closeDevice()
        worker.shutdown()
        super.onDestroy()
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        val uri = data?.data
        if (requestCode != REQUEST_FOLDER || resultCode != RESULT_OK || uri == null) return
        contentResolver.takePersistableUriPermission(
            uri, Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
        )
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(PREF_FOLDER, uri.toString()).apply()
        setOutputTree(uri)
    }

    // --- USB -----------------------------------------------------------------

    private fun connect() {
        val dev = usbManager.deviceList.values.firstOrNull { findMassStorageInterface(it) != null }
        if (dev == null) {
            setStatus(getString(R.string.no_drive))
            return
        }
        requestAndOpen(dev)
    }

    private fun requestAndOpen(dev: UsbDevice) {
        if (usbManager.hasPermission(dev)) {
            openDevice(dev)
            return
        }
        // The system fills in extras, so the PendingIntent must be mutable (and explicit).
        val flags = if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0
        val intent = Intent(ACTION_USB_PERMISSION).setPackage(packageName)
        usbManager.requestPermission(dev, PendingIntent.getBroadcast(this, 0, intent, flags))
    }

    private fun openDevice(dev: UsbDevice) {
        if (connection != null) return
        val ms = findMassStorageInterface(dev) ?: return setStatus(getString(R.string.no_drive))
        val conn = usbManager.openDevice(dev) ?: return setStatus("USB デバイスを開けませんでした")
        if (!conn.claimInterface(ms.usbInterface, true)) {
            conn.close()
            return setStatus("USB インターフェースを確保できませんでした")
        }
        device = dev
        connection = conn
        massStorage = ms
        textDrive.text = describeDevice(dev)
        setBusy(true)
        setStatus("ドライブを初期化しています…")

        worker.execute {
            try {
                val s = CdSession.open(
                    conn.fileDescriptor, ms.usbInterface.id, ms.bulkIn.address, ms.bulkOut.address
                )
                synchronized(sessionLock) { session = s }
                val name = s.inquiry()
                runOnUiThread {
                    driveName = name
                    textDrive.text = name
                }
                loadDisc(s)
            } catch (e: Exception) {
                runOnUiThread {
                    closeDevice()
                    setStatus("エラー: ${e.message}")
                    setBusy(false)
                }
            }
        }
    }

    /** Re-reads the TOC, e.g. after the disc was changed. */
    private fun reloadDisc() {
        setBusy(true)
        worker.execute {
            val s = session ?: return@execute
            try {
                loadDisc(s)
            } catch (e: Exception) {
                runOnUiThread {
                    setStatus("エラー: ${e.message}")
                    setBusy(false)
                }
            }
        }
    }

    // Worker thread: waits for the disc to spin up, reads the TOC, then looks
    // the disc up on CDDB.
    private fun loadDisc(s: CdSession) {
        runOnUiThread {
            toc = null
            metadata = null
            listTracks.adapter = null
            spinnerMatch.visibility = View.GONE
            textDisc.text = ""
            textResults.visibility = View.GONE
            setStatus("ディスクを待っています…")
        }
        var ready = s.isReady()
        var waited = 0
        while (!ready && waited < READY_WAIT_SECONDS) {
            Thread.sleep(1000)
            waited++
            ready = s.isReady()  // also clears UNIT ATTENTION after a disc change
        }
        if (!ready) throw IOException("ディスクが挿入されていないか、準備ができていません")
        val disc = s.readToc()
        runOnUiThread { showToc(disc) }
        lookUp(s, cddbEnabled, 0)
    }

    // Worker thread. A failed lookup only means generic names.
    private fun lookUp(s: CdSession, enabled: Boolean, matchIndex: Int) {
        if (enabled) runOnUiThread { setStatus("CDDB で検索しています…") }
        val meta = try {
            s.lookupCddb(enabled, CDDB_SERVER, matchIndex, http)
        } catch (e: Exception) {
            null
        }
        runOnUiThread {
            if (meta != null) showMetadata(meta)
            setBusy(false)
        }
    }

    /** After the CDDB switch or the chosen match changed. */
    private fun lookUpAgain(matchIndex: Int) {
        val enabled = cddbEnabled
        setBusy(true)
        worker.execute {
            val s = session
            if (s == null) {
                runOnUiThread { setBusy(false) }
                return@execute
            }
            lookUp(s, enabled, matchIndex)
        }
    }

    private fun cancelRip() {
        cancelRequested = true
        synchronized(sessionLock) { session?.cancel() }
    }

    private fun closeDevice() {
        val conn = connection ?: return
        val ms = massStorage
        connection = null
        massStorage = null
        device = null
        toc = null
        metadata = null
        listTracks.adapter = null
        spinnerMatch.visibility = View.GONE
        textDisc.text = ""
        buttonConnect.text = getString(R.string.connect)
        updateRipButton()
        // Queued after a running rip, which stops quickly once cancelled or unplugged.
        worker.execute {
            synchronized(sessionLock) {
                session?.close()
                session = null
            }
            if (ms != null) conn.releaseInterface(ms.usbInterface)
            conn.close()
        }
    }

    // --- Ripping -------------------------------------------------------------

    private fun startRip() {
        val disc = toc ?: return
        val tree = outputTree ?: return setStatus(getString(R.string.no_folder))
        val s = session ?: return
        val selected = disc.tracks.filterIndexed { i, t -> t.isAudio && listTracks.isItemChecked(i) }
        if (selected.isEmpty()) return setStatus("トラックを選択してください")
        val offset = editOffset.text.toString().trim().toIntOrNull()
            ?: return setStatus("読み取りオフセットは整数で指定してください")
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putInt(PREF_OFFSET, offset).apply()
        val verify = checkVerify.isChecked
        val format = selectedFormat()
        val accurateRip = checkAccurateRip.isChecked

        ripping = true
        cancelRequested = false
        setBusy(true)
        buttonRip.isEnabled = true
        buttonRip.text = getString(R.string.cancel)
        progress.progress = 0
        textResults.text = ""
        textResults.visibility = View.GONE

        worker.execute {
            val temp = File(cacheDir, "rip.$format")
            var message: String
            var results: String? = null
            try {
                s.beginRip(format, offset, MAX_RETRIES, verify)
                val dir = createDocument(treeDocument(tree), DocumentsContract.Document.MIME_TYPE_DIR, s.albumFolderName())
                var problems = 0
                for ((index, track) in selected.withIndex()) {
                    if (cancelRequested) throw CancellationException()
                    val label = "トラック ${track.number} (${index + 1}/${selected.size})"
                    runOnUiThread { setStatus("$label を読み取り中…") }
                    val r = s.ripTrack(track.number, temp.path) { done, total ->
                        val permille = if (total > 0) (1000L * done / total).toInt() else 0
                        runOnUiThread {
                            progress.progress = permille
                            setStatus("$label  ${permille / 10}%")
                        }
                    }
                    copyToDocument(temp, createDocument(dir, mimeType(format), s.trackFileName(track.number, format)))
                    if (!r.clean) problems++
                }
                if (accurateRip) runOnUiThread { setStatus("AccurateRip データベースを照会しています…") }
                val summary = s.checkAccurateRip(accurateRip, http)
                val log = s.ripLog()
                writeText(createDocument(dir, "application/octet-stream", "rip.log"), log)
                results = describeAccurateRip(summary)
                message = if (problems == 0) "完了しました (${selected.size} トラック)"
                else "完了しましたが、$problems トラックに読めないセクタがありました (rip.log を参照)"
            } catch (e: CancellationException) {
                message = "キャンセルしました"
            } catch (e: Exception) {
                message = "エラー: ${e.message}"
            } finally {
                temp.delete()
            }
            runOnUiThread {
                ripping = false
                buttonRip.text = getString(R.string.rip)
                setBusy(false)
                setStatus(message)
                if (results != null) {
                    textResults.text = results
                    textResults.scrollTo(0, 0)
                    textResults.visibility = View.VISIBLE
                }
            }
        }
    }

    private fun formatButtons(): List<Pair<String, RadioButton>> =
        listOf("flac" to radioFlac, "wav" to radioWav, "opus" to radioOpus, "vorbis" to radioVorbis, "mka" to radioMka)

    private fun selectedFormat(): String = formatButtons().firstOrNull { it.second.isChecked }?.first ?: "flac"

    // .opus is not in the MIME type table of older Android versions, whose
    // document providers would then append ".ogg" to the name; a generic type
    // keeps the name as given.
    private fun mimeType(format: String): String = when (format) {
        "flac" -> "audio/flac"
        "vorbis" -> "audio/ogg"
        "opus" -> "application/octet-stream"
        "mka" -> "audio/x-matroska"
        else -> "audio/x-wav"
    }

    // e.g. "Track 01: 一致 (v2) v2 12 / v1 0 / 15 件, プレス 1/2"
    private fun describeAccurateRip(summary: AccurateRipSummary): String {
        val lines = mutableListOf<String>()
        when (summary.status) {
            AccurateRipStatus.DISABLED -> lines += "AccurateRip: 照合しませんでした"
            AccurateRipStatus.NOT_FOUND -> lines += "AccurateRip: このディスクはデータベースに未登録です"
            AccurateRipStatus.ERROR -> lines += "AccurateRip: 照会に失敗しました (${summary.error})"
            AccurateRipStatus.FOUND -> {
                lines += "AccurateRip: %d / %d トラックが一致 (データベース登録 %d トラック, プレス %d 種)".format(
                    summary.accurateTracks, summary.tracks.size, summary.tracksInDatabase, summary.pressings
                )
                if (summary.accurateTracks == 0 && summary.tracksInDatabase > 0)
                    lines += "一致しません。読み取りオフセットを確認してください"
                for (t in summary.tracks) {
                    lines += when {
                        t.accurate -> "Track %02d: 一致 (%s) v2 %d / v1 %d / %d 件, プレス %d/%d".format(
                            t.number, t.matchedVersion, t.v2Confidence, t.v1Confidence, t.totalConfidence,
                            t.matchingPressings, t.pressings
                        )
                        t.inDatabase -> "Track %02d: 不一致 v2 0 / v1 0 / %d 件, プレス 0/%d".format(
                            t.number, t.totalConfidence, t.pressings
                        )
                        else -> "Track %02d: データベースに未登録".format(t.number)
                    }
                }
            }
        }
        return lines.joinToString("\n")
    }

    private fun treeDocument(tree: Uri): Uri =
        DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree))

    private fun createDocument(parent: Uri, mimeType: String, name: String): Uri =
        DocumentsContract.createDocument(contentResolver, parent, mimeType, name)
            ?: throw IOException("$name を作成できませんでした")

    // WAV and FLAC headers are patched at the end (and the Ogg pages are
    // simply written in one go), so the file is written
    // locally and then streamed through the document's ParcelFileDescriptor
    // (providers may hand out non-seekable pipes).
    private fun copyToDocument(source: File, document: Uri) {
        val pfd = contentResolver.openFileDescriptor(document, "w") ?: throw IOException("cannot open $document")
        pfd.use {
            FileOutputStream(it.fileDescriptor).use { out ->
                FileInputStream(source).use { input -> input.copyTo(out, 1 shl 16) }
            }
        }
    }

    private fun writeText(document: Uri, text: String) {
        contentResolver.openOutputStream(document)?.use { it.write(text.toByteArray()) }
    }

    // --- UI ------------------------------------------------------------------

    private fun showToc(disc: DiscToc) {
        toc = disc
        metadata = null
        showDiscInfo()
        showTracks(checked = disc.tracks.map { it.isAudio })
        setStatus("${disc.tracks.count { it.isAudio }} 個のオーディオトラック")
    }

    private fun showMetadata(meta: DiscMetadata) {
        val disc = toc ?: return
        metadata = meta
        showDiscInfo()
        showTracks(checked = disc.tracks.indices.map { listTracks.isItemChecked(it) })
        if (meta.matches.size > 1) {
            spinnerMatch.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, meta.matches)
            spinnerMatch.setSelection(meta.chosenMatch, false)
            spinnerMatch.visibility = View.VISIBLE
        } else {
            spinnerMatch.visibility = View.GONE
        }
        val audio = "${disc.tracks.count { it.isAudio }} 個のオーディオトラック"
        setStatus(
            when {
                meta.found && meta.matches.size > 1 -> "CDDB: ${meta.matches.size} 件の候補から選択できます  $audio"
                meta.found -> "CDDB: 見つかりました  $audio"
                meta.message == "disabled" -> audio
                else -> "CDDB: 見つかりませんでした (${meta.message})  $audio"
            }
        )
    }

    private fun showDiscInfo() {
        val disc = toc ?: return
        val meta = metadata
        val header = "%d トラック  全長 %s  CDDB %s".format(disc.tracks.size, formatMsf(disc.leadOutLba), disc.cddbHex)
        textDisc.text = if (meta != null && meta.found) {
            val year = if (meta.year.isNotEmpty()) " (${meta.year})" else ""
            "${meta.artist} / ${meta.album}$year\n$header"
        } else {
            header
        }
    }

    private fun showTracks(checked: List<Boolean>) {
        val disc = toc ?: return
        val labels = metadata?.trackLabels.orEmpty()
        val rows = disc.tracks.mapIndexed { i, t ->
            val kind = if (t.isAudio) "" else "  (データ)"
            val emphasis = if (t.preEmphasis) "  (プリエンファシス)" else ""
            val title = labels.getOrNull(i).orEmpty().let { if (it.isEmpty()) "" else "  $it" }
            "%02d   %s%s%s%s".format(t.number, formatMsf(t.lengthSectors), title, kind, emphasis)
        }
        listTracks.adapter = ArrayAdapter(this, android.R.layout.simple_list_item_multiple_choice, rows)
        disc.tracks.forEachIndexed { i, t -> listTracks.setItemChecked(i, t.isAudio && checked.getOrElse(i) { true }) }
    }

    private fun setOutputTree(uri: Uri) {
        outputTree = uri
        textFolder.text = uri.lastPathSegment ?: uri.toString()
        updateRipButton()
    }

    private fun setBusy(busy: Boolean) {
        buttonConnect.isEnabled = !busy
        buttonConnect.text = getString(if (connection == null) R.string.connect else R.string.reload)
        buttonFolder.isEnabled = !busy
        editOffset.isEnabled = !busy
        checkVerify.isEnabled = !busy
        checkCddb.isEnabled = !busy
        checkAccurateRip.isEnabled = !busy
        for ((_, radio) in formatButtons()) radio.isEnabled = !busy
        spinnerMatch.isEnabled = !busy
        listTracks.isEnabled = !busy
        updateRipButton(busy)
    }

    private fun updateRipButton(busy: Boolean = false) {
        buttonRip.isEnabled = ripping || (!busy && toc != null && outputTree != null)
    }

    private fun setStatus(text: String) {
        textStatus.text = text
    }

    private fun applySystemBarInsets(root: View) {
        // targetSdk 35 draws edge to edge: keep the controls out of the system bars.
        if (Build.VERSION.SDK_INT < 30) return
        val base = root.paddingTop
        root.setOnApplyWindowInsetsListener { v, insets ->
            val bars = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.ime())
            v.setPadding(base + bars.left, base + bars.top, base + bars.right, base + bars.bottom)
            insets
        }
    }

    private fun Intent.usbDevice(): UsbDevice? =
        if (Build.VERSION.SDK_INT >= 33) getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
        else @Suppress("DEPRECATION") getParcelableExtra(UsbManager.EXTRA_DEVICE)
}
