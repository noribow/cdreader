package io.github.noribow.cdreader

import android.app.Activity
import android.app.AlertDialog
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.SharedPreferences
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.DocumentsContract
import android.security.NetworkSecurityPolicy
import android.text.Editable
import android.text.TextWatcher
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
import android.widget.Switch
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
 * CDDB), rip the selected tracks to FLAC, Ogg FLAC, WAV, Opus or Vorbis files (or into one file with a
 * CUE sheet, #42) in a folder chosen with the Storage Access Framework and check them against AccurateRip.
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
        const val PREF_C2 = "useC2"
        const val PREF_CACHE = "cacheMode"
        const val PREF_ADVANCED = "advancedOpen"
        // Single-file mode (#42): the whole selection as one file plus a .cue file.
        const val PREF_SINGLE_FILE = "singleFile"
        // CDDB server and contact e-mail address (#38); "" = default server / anonymous greeting.
        // The server is a preset id ("gnudb", "japdb") or a custom URL (#46); earlier versions
        // saved URLs only, see CddbSettings.server().
        const val PREF_CDDB_SERVER = "cddbServer"
        // The text of the custom URL field, kept while a preset is chosen (#46).
        const val PREF_CDDB_CUSTOM_SERVER = "cddbCustomServer"
        const val PREF_CDDB_EMAIL = "cddbEmail"
        // Detected read offsets per drive model (#37): key = DriveInfo.offsetKey.
        const val DRIVE_OFFSETS = "driveOffsets"
        const val DRIVE_OFFSET_NOTES = "driveOffsetNotes"
        // AccurateRip drive offset database (#37), in cacheDir; refreshed after 30 days.
        const val DRIVE_OFFSET_DB_FILE = "DriveOffsets.bin"
        const val MAX_RETRIES = 5
        const val READY_WAIT_SECONDS = 30
        const val USER_AGENT = "cdreader/0.1.0 (Android)"
    }

    private lateinit var usbManager: UsbManager
    private val worker: ExecutorService = Executors.newSingleThreadExecutor()
    // The CDDB connection test needs no drive: it does not wait for a disc being loaded.
    private val cddbTestWorker: ExecutorService = Executors.newSingleThreadExecutor()
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
    private lateinit var textOffsetSource: TextView
    private lateinit var buttonDetectOffset: Button
    private lateinit var checkVerify: CheckBox
    private lateinit var checkC2: CheckBox
    private lateinit var spinnerCache: Spinner
    private lateinit var switchSingleFile: Switch
    private lateinit var textAdvanced: TextView
    private lateinit var groupAdvanced: View
    private lateinit var checkCddb: CheckBox
    private lateinit var checkAccurateRip: CheckBox
    private lateinit var groupFormat: RadioGroup
    private lateinit var radioFlac: RadioButton
    private lateinit var radioOggFlac: RadioButton
    private lateinit var radioWav: RadioButton
    private lateinit var radioOpus: RadioButton
    private lateinit var radioVorbis: RadioButton
    private lateinit var radioMka: RadioButton
    private lateinit var radioAlac: RadioButton
    private lateinit var progress: ProgressBar
    private lateinit var spinnerCddbServer: Spinner
    private lateinit var textCddbServerUrl: TextView
    private lateinit var editCddbServer: EditText
    private lateinit var editCddbEmail: EditText
    private lateinit var buttonCddbTest: Button
    private lateinit var textCddbTest: TextView

    // Owned by the worker thread once opened.
    private var device: UsbDevice? = null
    private var connection: UsbDeviceConnection? = null
    private var massStorage: MassStorageInterface? = null
    @Volatile private var session: CdSession? = null
    private val sessionLock = Any()  // cancel() from the UI thread vs. close() on the worker

    // Read by the worker when a disc is loaded.
    @Volatile private var cddbEnabled = true
    // The saved (valid) CDDB settings (#38); "" = default server / anonymous greeting.
    // The server is a preset id or a custom URL (#46), passed to native code as it is.
    @Volatile private var cddbServer = ""
    @Volatile private var cddbEmail = ""
    // Set on the UI thread; stops a rip between tracks, even before it reached native code.
    @Volatile private var cancelRequested = false

    // UI thread state.
    private var driveName = ""
    private var driveInfo: DriveInfo? = null
    // The drive's entry in the AccurateRip drive offset database, if known (#37).
    private var driveDb: DriveOffsetDb? = null
    // Where the value in editOffset came from (#37); a manual edit makes it MANUAL.
    private var offsetSource = OffsetSource.MANUAL
    private var offsetDetail = ""
    private var settingOffset = false  // editOffset changed by the app, not the user
    private var detecting = false
    // Asked once per connected drive whether to detect the offset before ripping.
    private var askedOffset = false
    private var toc: DiscToc? = null
    private var metadata: DiscMetadata? = null
    private var outputTree: Uri? = null
    private var ripping = false
    private var busy = false
    private var testingCddb = false
    // The "set a contact e-mail address" dialog was shown for the current settings.
    private var emailHintShown = false

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
        textOffsetSource = findViewById(R.id.textOffsetSource)
        buttonDetectOffset = findViewById(R.id.buttonDetectOffset)
        checkVerify = findViewById(R.id.checkVerify)
        checkC2 = findViewById(R.id.checkC2)
        spinnerCache = findViewById(R.id.spinnerCache)
        switchSingleFile = findViewById(R.id.switchSingleFile)
        textAdvanced = findViewById(R.id.textAdvanced)
        groupAdvanced = findViewById(R.id.groupAdvanced)
        checkCddb = findViewById(R.id.checkCddb)
        checkAccurateRip = findViewById(R.id.checkAccurateRip)
        groupFormat = findViewById(R.id.groupFormat)
        radioFlac = findViewById(R.id.radioFlac)
        radioOggFlac = findViewById(R.id.radioOggFlac)
        radioWav = findViewById(R.id.radioWav)
        radioOpus = findViewById(R.id.radioOpus)
        radioVorbis = findViewById(R.id.radioVorbis)
        radioMka = findViewById(R.id.radioMka)
        radioAlac = findViewById(R.id.radioAlac)
        progress = findViewById(R.id.progress)
        spinnerCddbServer = findViewById(R.id.spinnerCddbServer)
        textCddbServerUrl = findViewById(R.id.textCddbServerUrl)
        editCddbServer = findViewById(R.id.editCddbServer)
        editCddbEmail = findViewById(R.id.editCddbEmail)
        buttonCddbTest = findViewById(R.id.buttonCddbTest)
        textCddbTest = findViewById(R.id.textCddbTest)
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
        checkC2.isChecked = prefs.getBoolean(PREF_C2, true)
        spinnerCache.setSelection(CacheMode.fromKey(prefs.getString(PREF_CACHE, null)).ordinal, false)
        switchSingleFile.isChecked = prefs.getBoolean(PREF_SINGLE_FILE, false)
        showAdvanced(prefs.getBoolean(PREF_ADVANCED, false))
        loadCddbServer(prefs)
        cddbEmail = prefs.getString(PREF_CDDB_EMAIL, "").orEmpty()
        editCddbEmail.setText(cddbEmail)

        buttonConnect.setOnClickListener { if (connection == null) connect() else reloadDisc() }
        buttonFolder.setOnClickListener {
            startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQUEST_FOLDER)
        }
        buttonRip.setOnClickListener { if (ripping) cancelRip() else startRip() }
        buttonDetectOffset.setOnClickListener { if (detecting) cancelRip() else startDetectOffset(thenRip = false) }
        editOffset.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(text: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(text: CharSequence?, start: Int, before: Int, count: Int) {}
            override fun afterTextChanged(text: Editable?) {
                if (settingOffset) return
                // Typed by the user: a manual value from now on.
                offsetSource = OffsetSource.MANUAL
                offsetDetail = ""
                showOffsetSource("")
            }
        })
        groupFormat.setOnCheckedChangeListener { _, _ ->
            prefs.edit().putString(PREF_FORMAT, selectedFormat()).apply()
        }
        checkAccurateRip.setOnCheckedChangeListener { _, checked ->
            prefs.edit().putBoolean(PREF_ACCURATERIP, checked).apply()
        }
        checkC2.setOnCheckedChangeListener { _, checked ->
            prefs.edit().putBoolean(PREF_C2, checked).apply()
        }
        switchSingleFile.setOnCheckedChangeListener { _, checked ->
            prefs.edit().putBoolean(PREF_SINGLE_FILE, checked).apply()
        }
        spinnerCache.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                prefs.edit().putString(PREF_CACHE, selectedCacheMode().key).apply()
            }

            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }
        textAdvanced.setOnClickListener {
            val open = groupAdvanced.visibility != View.VISIBLE
            showAdvanced(open)
            prefs.edit().putBoolean(PREF_ADVANCED, open).apply()
        }
        spinnerCddbServer.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                // Also called once for the initial selection: saving is a no-op then.
                showCddbServerChoice()
                saveCddbSettings()
            }

            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }
        editCddbServer.addTextChangedListener(afterChange {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                .putString(PREF_CDDB_CUSTOM_SERVER, editCddbServer.text.toString().trim()).apply()
            saveCddbSettings()
        })
        editCddbEmail.addTextChangedListener(afterChange { saveCddbSettings() })
        buttonCddbTest.setOnClickListener { startCddbTest() }
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
        cddbTestWorker.shutdown()
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
                val info = s.driveInfo()
                // AccurateRip drive offset database (#37): only the stored copy here, no network.
                s.setDriveOffsetDbCache(File(cacheDir, DRIVE_OFFSET_DB_FILE))
                val db = runCatching { s.lookupDriveOffsetDb(null) }.getOrNull()
                runOnUiThread {
                    driveName = info.displayName
                    driveInfo = info
                    driveDb = db
                    askedOffset = false
                    textDrive.text = info.displayName
                    applySavedOffset()
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
            s.lookupCddb(enabled, cddbServer, cddbEmail, matchIndex, http)
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
        driveInfo = null
        toc = null
        metadata = null
        listTracks.adapter = null
        spinnerMatch.visibility = View.GONE
        textDisc.text = ""
        buttonConnect.text = getString(R.string.connect)
        if (!detecting) buttonDetectOffset.isEnabled = false
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

    /** [askOffset]: ask first whether to detect the offset when none is set for the drive (#37). */
    private fun startRip(askOffset: Boolean = true) {
        val disc = toc ?: return
        val tree = outputTree ?: return setStatus(getString(R.string.no_folder))
        val s = session ?: return
        val selected = disc.tracks.filterIndexed { i, t -> t.isAudio && listTracks.isItemChecked(i) }
        if (selected.isEmpty()) return setStatus("トラックを選択してください")
        val offset = editOffset.text.toString().trim().toIntOrNull()
        if (offset == null) {
            showAdvanced(true)  // the field is in 詳細設定
            return setStatus("読み取りオフセットは整数で指定してください")
        }
        val info = driveInfo
        if (askOffset && !askedOffset && offset == 0 && info != null && savedOffset(info) == null) {
            AlertDialog.Builder(this)
                .setTitle("読み取りオフセットが未設定です")
                .setMessage(
                    "このドライブ (${info.product}) の読み取りオフセットは保存されていません。" +
                        "オフセット 0 のままだと AccurateRip で一致しないことがあります。\n\n" +
                        "リッピングの前に AccurateRip を使ってオフセットを自動検出しますか? " +
                        "(数トラックを読み取ります。ディスクが AccurateRip に登録されている必要があります)"
                )
                .setPositiveButton("検出する") { _, _ ->
                    askedOffset = true
                    startDetectOffset(thenRip = true)
                }
                .setNegativeButton("このまま続ける") { _, _ ->
                    askedOffset = true
                    startRip(askOffset = false)
                }
                .show()
            return
        }
        // Only a typed value is the app-wide default; saved / detected ones belong to the drive.
        if (offsetSource == OffsetSource.MANUAL)
            getSharedPreferences(PREFS, MODE_PRIVATE).edit().putInt(PREF_OFFSET, offset).apply()
        val source = offsetSource
        val sourceDetail = offsetDetail
        val verify = checkVerify.isChecked
        val useC2 = checkC2.isChecked
        val cacheMode = selectedCacheMode()
        val format = selectedFormat()
        val accurateRip = checkAccurateRip.isChecked
        val singleFile = switchSingleFile.isChecked

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
            val tempCue = File(cacheDir, "rip.cue")
            var message: String
            var results: String? = null
            var suggestDetection = false
            try {
                // Single-file mode (#42): only consecutive tracks make an image of the disc.
                val image = if (singleFile) imageNamesOrExplain(s, format, selected) else null
                if (image != null) checkImageSpace(disc, selected, image, format)
                if (cacheMode == CacheMode.AUTO) runOnUiThread { setStatus("ドライブのキャッシュを確認しています…") }
                // The drive database's offset for rip.log (#37); online only with AccurateRip enabled.
                runCatching { s.lookupDriveOffsetDb(if (accurateRip) http else null) }
                s.beginRip(format, offset, MAX_RETRIES, verify, useC2, cacheMode, source, sourceDetail)
                val ripped = mutableListOf<Pair<Int, RipResult>>()
                var imageResult: ImageResult? = null
                val dir: Uri
                if (image != null) {
                    val (folder, r) = ripImage(s, tree, image, format, selected, temp, tempCue)
                    dir = folder
                    imageResult = r
                    ripped += r.parts
                } else {
                    dir = createDocument(treeDocument(tree), DocumentsContract.Document.MIME_TYPE_DIR, s.albumFolderName())
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
                        ripped += track.number to r
                    }
                }
                val problems = ripped.count { !it.second.clean }
                if (accurateRip) runOnUiThread { setStatus("AccurateRip データベースを照会しています…") }
                val summary = s.checkAccurateRip(accurateRip, http)
                val log = s.ripLog()
                writeText(createDocument(dir, "application/octet-stream", "rip.log"), log)
                results = (if (image != null && imageResult != null) describeImage(image, imageResult) + "\n" else "") +
                    describeC2(s.c2Status(), ripped) + "\n" + describeCache(s.cacheStatus(), ripped) + "\n" +
                    describeAccurateRip(summary)
                // The disc is in AccurateRip but nothing matched: most likely the offset (#37).
                suggestDetection = summary.status == AccurateRipStatus.FOUND && summary.accurateTracks == 0 &&
                    summary.tracksInDatabase > 0
                val what = if (image != null) "${selected.size} トラックを 1 ファイルに保存" else "${selected.size} トラック"
                message = if (problems == 0) "完了しました ($what)"
                else "完了しましたが、$problems トラックに読めないセクタまたは疑わしい位置がありました (rip.log を参照)"
            } catch (e: CancellationException) {
                message = "キャンセルしました"
            } catch (e: Exception) {
                message = "エラー: ${e.message}"
            } finally {
                temp.delete()
                tempCue.delete()
            }
            finishRip(message, results, suggestDetection)
        }
    }

    // Worker thread: shows the outcome of a rip on the UI thread.
    private fun finishRip(message: String, results: String?, suggestDetection: Boolean) {
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
            if (suggestDetection && toc != null) suggestOffsetDetection()
        }
    }

    // --- Single-file mode (#42) -----------------------------------------------

    /** Worker thread: the names of the image, or an IOException that says what to select. */
    private fun imageNamesOrExplain(s: CdSession, format: String, selected: List<TrackInfo>): ImageNames =
        try {
            s.imageFileNames(format, selected.map { it.number })
        } catch (e: IllegalArgumentException) {
            throw IOException(
                "1 ファイルにするには、連続したオーディオトラックを選択してください (${e.message})"
            )
        }

    /**
     * The image is written to the app's cache first (the writers patch their
     * headers at the end): a whole disc as WAV needs up to about 800 MB there.
     */
    private fun checkImageSpace(disc: DiscToc, selected: List<TrackInfo>, image: ImageNames, format: String) {
        var sectors = selected.sumOf { it.lengthSectors.toLong() }
        if (image.withHtoa) sectors += disc.tracks.firstOrNull { it.number == 1 }?.startLba?.toLong() ?: 0L
        val pcmBytes = sectors * 2352
        // Rough upper bounds of the encoded size (lossless compression rarely saves less than 20%).
        val needed = when (format) {
            "wav" -> pcmBytes
            "opus", "vorbis" -> pcmBytes / 5
            else -> pcmBytes * 4 / 5
        } + (16L shl 20)
        val free = cacheDir.usableSpace
        if (free < needed) {
            throw IOException(
                "端末の空き容量が足りません (一時ファイルに約 %d MB 必要、空き %d MB)".format(needed shr 20, free shr 20)
            )
        }
    }

    /**
     * Worker thread: rips the image into [temp] / [tempCue], then copies both
     * into a new album folder, which is returned with the result. The folder
     * is created only after the rip and removed again (with what was copied
     * into it) when the copy fails or is cancelled: no partial image is left.
     */
    private fun ripImage(
        s: CdSession,
        tree: Uri,
        image: ImageNames,
        format: String,
        selected: List<TrackInfo>,
        temp: File,
        tempCue: File,
    ): Pair<Uri, ImageResult> {
        runOnUiThread { setStatus("${image.imageFile} を読み取り中…") }
        val r = s.ripImage(selected.map { it.number }, temp.path, tempCue.path) { track, done, total ->
            val permille = if (total > 0) (1000L * done / total).toInt() else 0
            val part = if (track == 0) "隠しトラック (HTOA)" else "トラック $track"
            runOnUiThread {
                progress.progress = permille
                setStatus("1 ファイルに保存: $part を読み取り中  ${permille / 10}%")
            }
        }
        if (cancelRequested) throw CancellationException()
        runOnUiThread { setStatus("${image.imageFile} を保存しています…") }
        val dir = createDocument(treeDocument(tree), DocumentsContract.Document.MIME_TYPE_DIR, s.albumFolderName())
        try {
            copyToDocument(temp, createDocument(dir, mimeType(format), image.imageFile))
            copyToDocument(tempCue, createDocument(dir, "application/octet-stream", image.cueFile))
        } catch (e: Exception) {
            deleteDocument(dir)
            throw e
        }
        return dir to r
    }

    // e.g. "1 ファイル: Artist - Album.flac + Artist - Album.cue (CUE シートをファイル内にも埋め込み)"
    private fun describeImage(image: ImageNames, r: ImageResult): String {
        val embedded = when {
            !r.embeddedCue -> ""
            image.imageFile.endsWith(".mka") -> " (トラックはファイル内のチャプターにも記録)"
            else -> " (CUE シートをファイル内にも埋め込み)"
        }
        var text = "1 ファイル: ${image.imageFile} + ${image.cueFile}$embedded"
        if (r.withHtoa) text += "\n先頭に隠しトラック (HTOA) を含みます (Track 00)"
        return text + "\nCRC32 %08X".format(r.crc32)
    }

    // --- Read offset (#37) -----------------------------------------------------

    private fun driveOffsetPrefs() = getSharedPreferences(DRIVE_OFFSETS, MODE_PRIVATE)
    private fun driveOffsetNotePrefs() = getSharedPreferences(DRIVE_OFFSET_NOTES, MODE_PRIVATE)

    /** The offset saved for this drive model, or null. */
    private fun savedOffset(info: DriveInfo): Int? {
        val prefs = driveOffsetPrefs()
        return if (prefs.contains(info.offsetKey)) prefs.getInt(info.offsetKey, 0) else null
    }

    private fun saveOffset(info: DriveInfo, offset: Int, note: String) {
        driveOffsetPrefs().edit().putInt(info.offsetKey, offset).apply()
        driveOffsetNotePrefs().edit().putString(info.offsetKey, note).apply()
    }

    /** Sets the field without making it a manual value. */
    private fun setOffsetField(offset: Int, source: OffsetSource, detail: String, label: String) {
        settingOffset = true
        editOffset.setText(offset.toString())
        settingOffset = false
        offsetSource = source
        offsetDetail = detail
        showOffsetSource(label)
    }

    private fun showOffsetSource(label: String) {
        textOffsetSource.text = label
        textOffsetSource.visibility = if (label.isEmpty()) View.GONE else View.VISIBLE
    }

    /** A newly connected drive: its saved offset, otherwise the last value typed. */
    private fun applySavedOffset() {
        val info = driveInfo ?: return
        val saved = savedOffset(info)
        if (saved != null) {
            val note = driveOffsetNotePrefs().getString(info.offsetKey, "").orEmpty()
            val detail = info.displayName + if (note.isEmpty()) "" else "; $note"
            setOffsetField(saved, OffsetSource.SAVED, detail, "(ドライブ ${info.product} の保存値)" + driveDbLabel(saved))
        } else if (offsetSource != OffsetSource.MANUAL) {
            // The value of another drive: back to the app-wide manual value.
            val manual = getSharedPreferences(PREFS, MODE_PRIVATE).getInt(PREF_OFFSET, 0)
            setOffsetField(manual, OffsetSource.MANUAL, "", driveDbLabel(manual).trim())
        } else {
            showOffsetSource(driveDbLabel(editOffset.text.toString().trim().toIntOrNull()).trim())
        }
    }

    /** " (AccurateRip ドライブ DB: +6, 1234 件)" when the stored database lists the drive, else "". */
    private fun driveDbLabel(current: Int?): String {
        val db = driveDb ?: return ""
        if (db.status != DriveDbStatus.FOUND) return ""
        val count = if (db.submissions >= 0) ", ${db.submissions} 件" else ""
        val differs = if (current != null && current != db.offset) " — 現在の値と異なります" else ""
        return " (AccurateRip ドライブ DB: %+d%s)%s".format(db.offset, count, differs)
    }

    private fun suggestOffsetDetection() {
        AlertDialog.Builder(this)
            .setTitle("AccurateRip で一致したトラックがありません")
            .setMessage(
                "このディスクは AccurateRip に登録されていますが、どのトラックも一致しませんでした。" +
                    "読み取りオフセットが合っていない可能性があります。\n\nオフセットを自動検出しますか?"
            )
            .setPositiveButton("検出する") { _, _ -> startDetectOffset(thenRip = false) }
            .setNegativeButton("後で", null)
            .show()
    }

    /** Detects the offset of the drive with the disc in it; [thenRip]: start the rip after a confirmed result. */
    private fun startDetectOffset(thenRip: Boolean) {
        if (toc == null || ripping || detecting) return
        val s = session ?: return
        val info = driveInfo ?: return
        detecting = true
        cancelRequested = false
        setBusy(true)
        showAdvanced(true)
        progress.progress = 0
        setStatus("AccurateRip でオフセットを検出しています…")
        worker.execute {
            var error: String? = null
            val result = try {
                s.detectOffset(http) { step, steps, track, done, total ->
                    val part = if (total > 0) 1000L * done / total else 0L
                    val permille = (((step - 1) * 1000L + part) / steps.coerceAtLeast(1)).toInt()
                    runOnUiThread {
                        progress.progress = permille
                        setStatus("オフセット検出: トラック $track を読み取り中 ($step/$steps)  ${part / 10}%")
                    }
                }
            } catch (e: Exception) {
                error = e.message
                null
            }
            runOnUiThread {
                detecting = false
                setBusy(false)
                if (result != null) finishDetection(info, result, thenRip) else setStatus("エラー: $error")
            }
        }
    }

    private fun finishDetection(info: DriveInfo, d: OffsetDetection, thenRip: Boolean) {
        if (driveInfo?.offsetKey != info.offsetKey) return  // the drive was unplugged meanwhile
        if (d.status == OffsetDetectStatus.DETECTED) {
            val db = if (d.matchesDriveDb) ", ドライブ DB と一致" else ""
            val agreed = "${d.agreeingTracks}/${d.testedTracks} トラック一致, ${d.version}$db"
            setOffsetField(d.offset, OffsetSource.DETECTED, d.agreement, "(自動検出: $agreed)")
            saveOffset(info, d.offset, "auto-detected: ${d.agreement}")
            val single = if (d.singleTrack && !d.matchesDriveDb)
                " 登録トラックが 1 つだけのディスクのため、別の CD でも確認することをおすすめします。" else ""
            val others = if (d.alternatives.isEmpty()) "" else
                " 他のプレスの候補: ${d.alternatives.joinToString(", ") { "%+d".format(it.offset) }}"
            val decided = if (d.decidedByDriveDb)
                " AccurateRip のドライブ DB の登録値%sと一致したため確定しました。"
                    .format(if (d.driveDbSubmissions >= 0) " (${d.driveDbSubmissions} 件)" else "")
            else ""
            setStatus("オフセット %+d を検出しました (%s)。ドライブ %s の値として保存しました。%s%s%s".format(
                d.offset, agreed, info.product, decided, single, others
            ))
            if (thenRip) startRip(askOffset = false)
            return
        }
        val message = describeDetectionFailure(d)
        setStatus(message)
        if (d.status == OffsetDetectStatus.AMBIGUOUS && d.allCandidates.isNotEmpty()) {
            chooseOffsetCandidate(info, d, message, thenRip)
            return
        }
        if (thenRip && d.status != OffsetDetectStatus.CANCELLED) {
            AlertDialog.Builder(this)
                .setTitle("オフセットを検出できませんでした")
                .setMessage("$message\n\n現在のオフセット (${editOffset.text}) のままリッピングしますか?")
                .setPositiveButton("リッピングする") { _, _ -> startRip(askOffset = false) }
                .setNegativeButton("やめる", null)
                .show()
        }
    }

    /**
     * Ambiguous detection (#37): the candidates as a list to pick from. The
     * chosen offset is applied and saved for the drive ("selected by user from
     * candidates ...").
     */
    private fun chooseOffsetCandidate(info: DriveInfo, d: OffsetDetection, message: String, thenRip: Boolean) {
        val dbFound = d.driveDbStatus == DriveDbStatus.FOUND
        val items = d.allCandidates.map { c ->
            val pressings = if (c.pressings.isEmpty()) "" else ", プレス ${c.pressings.joinToString("+")}"
            val marker = if (dbFound && c.offset == d.driveDbOffset) "  ★ドライブ DB の登録値" else ""
            "%+d: %d トラック一致, 登録 %d 件%s%s".format(c.offset, c.tracks, c.confidence, pressings, marker)
        }.toTypedArray()
        AlertDialog.Builder(this)
            .setTitle("オフセットの候補から選択")
            .setItems(items) { _, which ->
                val chosen = d.allCandidates[which].offset
                if (driveInfo?.offsetKey != info.offsetKey) return@setItems
                setOffsetField(chosen, OffsetSource.SELECTED, d.candidateList, "(候補から選択)")
                saveOffset(info, chosen, d.selectionNote)
                setStatus("オフセット %+d を選択し、ドライブ %s の値として保存しました".format(chosen, info.product))
                if (thenRip) startRip(askOffset = false)
            }
            .setNegativeButton(if (thenRip) "選ばずにリッピング" else "選ばない") { _, _ ->
                if (thenRip) startRip(askOffset = false)
            }
            .show()
        // The explanation stays in the status line (a list dialog shows no message).
        setStatus("$message\n候補から正しいと思うオフセットを選んでください (同じドライブで別の CD を検出すると確定できることがあります)")
    }

    /** What the AccurateRip drive database says when detection did not confirm an offset. */
    private fun driveDbNote(d: OffsetDetection): String = when (d.driveDbStatus) {
        DriveDbStatus.FOUND -> {
            val found = d.allCandidates.any { it.offset == d.driveDbOffset } || d.candidates.contains(d.driveDbOffset) ||
                (d.agreeingTracks > 0 && d.offset == d.driveDbOffset)
            if (found) "" else "\nAccurateRip のドライブ DB の登録値は %+d%s ですが、候補に含まれていません".format(
                d.driveDbOffset, if (d.driveDbSubmissions >= 0) " (${d.driveDbSubmissions} 件)" else ""
            )
        }
        DriveDbStatus.NOT_LISTED -> "\nAccurateRip のドライブ DB にこのドライブの登録はありません"
        DriveDbStatus.UNAVAILABLE -> "\nAccurateRip のドライブ DB を取得できませんでした"
        DriveDbStatus.NOT_CHECKED -> ""
    }

    private fun describeDetectionFailure(d: OffsetDetection): String =
        describeDetectionStatus(d) + if (d.status == OffsetDetectStatus.CANCELLED) "" else driveDbNote(d)

    private fun describeDetectionStatus(d: OffsetDetection): String = when (d.status) {
        OffsetDetectStatus.DETECTED -> d.summary
        OffsetDetectStatus.NOT_IN_DATABASE ->
            "このディスクは AccurateRip に登録されていないため、オフセットを検出できません。よく知られた別の CD で試してください"
        OffsetDetectStatus.LOOKUP_FAILED -> "AccurateRip の照会に失敗しました (${d.error})"
        OffsetDetectStatus.NO_USABLE_TRACKS ->
            "AccurateRip にこのディスクのトラックの登録がないため、オフセットを検出できません"
        OffsetDetectStatus.NO_MATCH ->
            "±3000 サンプルの範囲で一致するオフセットが見つかりませんでした (${d.testedTracks} トラックを照合)。別の CD で試してください"
        OffsetDetectStatus.NOT_ENOUGH ->
            if (d.usableTracks <= 1)
                "オフセット %+d で一致しましたが、AccurateRip の登録件数が少ない (%d 件) ため確定できません。別の CD で試してください"
                    .format(d.offset, d.confidence)
            else
                "オフセット %+d で一致したのは %d / %d トラックだけのため確定できません。別の CD で試してください"
                    .format(d.offset, d.agreeingTracks, d.testedTracks)
        OffsetDetectStatus.CONFLICT ->
            "トラックごとに一致するオフセットが異なるため確定できません (候補: %s)。別の CD で試してください"
                .format(d.candidates.joinToString(", ") { "%+d".format(it) })
        OffsetDetectStatus.CANCELLED -> "オフセットの検出を中止しました"
        OffsetDetectStatus.AMBIGUOUS ->
            ("どのトラックも同じ複数のオフセットで一致しました (互いにずれた別プレスの登録)。" +
                "登録件数に十分な差がないため確定できません (候補: %s)")
                .format(
                    (listOf(OffsetAlternative(d.offset, d.confidence)) + d.alternatives)
                        .joinToString(", ") { "%+d (%d 件)".format(it.offset, it.confidence) }
                )
    }

    // --- CDDB settings (#38) ---------------------------------------------------

    private fun afterChange(action: () -> Unit) = object : TextWatcher {
        override fun beforeTextChanged(text: CharSequence?, start: Int, count: Int, after: Int) {}
        override fun onTextChanged(text: CharSequence?, start: Int, before: Int, count: Int) {}
        override fun afterTextChanged(text: Editable?) = action()
    }

    /** Japanese text for a problem of the typed settings, null if none. */
    private fun cddbProblemText(problem: CddbSettingProblem): String? = when (problem) {
        CddbSettingProblem.NONE -> null
        CddbSettingProblem.SERVER_SCHEME -> "http:// または https:// で始まる URL を入力してください"
        CddbSettingProblem.SERVER_HOST -> "URL のホスト名 (またはポート番号) が正しくありません"
        CddbSettingProblem.SERVER_CHARACTERS -> "URL に空白・制御文字・全角文字・# は使えません"
        CddbSettingProblem.EMAIL_SHAPE -> "user@example.com の形式で入力してください (半角英数字)"
        CddbSettingProblem.HELLO_CHARACTERS -> "使えない文字が含まれています"
    }

    /**
     * The server dropdown (#46): GNUDB / JAPDB / カスタム. Settings saved by earlier
     * versions hold a URL: one of a preset becomes that preset, any other the custom URL.
     */
    private fun loadCddbServer(prefs: SharedPreferences) {
        val saved = prefs.getString(PREF_CDDB_SERVER, "").orEmpty()
        val setting = CddbSettings.server(saved)
        var customUrl = prefs.getString(PREF_CDDB_CUSTOM_SERVER, null)
        val edit = prefs.edit()
        if (setting.choice == CddbSettings.CUSTOM && customUrl == null) {
            customUrl = setting.value
            edit.putString(PREF_CDDB_CUSTOM_SERVER, customUrl)
        }
        // "" (nothing saved) is the default preset.
        cddbServer = if (setting.choice == CddbSettings.CUSTOM) setting.value else setting.choice
        if (cddbServer != saved) edit.putString(PREF_CDDB_SERVER, cddbServer)
        edit.apply()

        val labels = CddbSettings.presets.map { it.label } + getString(R.string.cddb_server_custom)
        val adapter = ArrayAdapter(this, android.R.layout.simple_spinner_item, labels)
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        spinnerCddbServer.adapter = adapter
        val index = CddbSettings.presets.indexOfFirst { it.id == setting.choice }
        spinnerCddbServer.setSelection(if (index >= 0) index else CddbSettings.presets.size, false)
        editCddbServer.setText(if (setting.choice == CddbSettings.CUSTOM) setting.value else customUrl.orEmpty())
        showCddbServerChoice()
    }

    /** The preset chosen in the dropdown, null for カスタム. */
    private fun selectedCddbPreset(): CddbServerPreset? =
        CddbSettings.presets.getOrNull(spinnerCddbServer.selectedItemPosition)

    /** The URL field only for カスタム, the preset's URL below the dropdown otherwise. */
    private fun showCddbServerChoice() {
        val preset = selectedCddbPreset()
        editCddbServer.visibility = if (preset == null) View.VISIBLE else View.GONE
        textCddbServerUrl.visibility = if (preset == null) View.GONE else View.VISIBLE
        textCddbServerUrl.text = preset?.url.orEmpty()
    }

    /** The server setting as chosen: a preset id or the custom URL as typed. */
    private fun selectedCddbServer(): String = selectedCddbPreset()?.id ?: editCddbServer.text.toString().trim()

    /**
     * The custom URL is http:// and this app's network security config blocks plain
     * HTTP to its host (only the JAPDB server and AccurateRip are allowed, #46).
     */
    private fun cleartextBlocked(url: String): Boolean {
        if (!url.startsWith("http://", ignoreCase = true)) return false
        val host = Uri.parse(url).host ?: return false
        return !NetworkSecurityPolicy.getInstance().isCleartextTrafficPermitted(host)
    }

    /** Japanese text for a problem of the server setting as chosen, null if none. */
    private fun cddbServerProblem(server: String, check: CddbSettingsCheck): String? = when {
        selectedCddbPreset() != null -> null
        server.isEmpty() -> getString(R.string.cddb_server_url_needed)
        cddbProblemText(check.server) != null -> cddbProblemText(check.server)
        cleartextBlocked(server) -> getString(R.string.cddb_cleartext_blocked)
        else -> null
    }

    /** Saves the fields when valid (the last valid values stay in use otherwise) and marks invalid ones. */
    private fun saveCddbSettings() {
        val server = selectedCddbServer()
        val email = editCddbEmail.text.toString().trim()
        val check = CddbSettings.check(server, email)
        val serverProblem = cddbServerProblem(server, check)
        val emailProblem = cddbProblemText(check.email)
        editCddbServer.error = if (selectedCddbPreset() == null) serverProblem else null
        editCddbEmail.error = emailProblem
        val edit = getSharedPreferences(PREFS, MODE_PRIVATE).edit()
        if (serverProblem == null && server != cddbServer) {
            cddbServer = server
            emailHintShown = false
            edit.putString(PREF_CDDB_SERVER, server)
        }
        if (emailProblem == null && email != cddbEmail) {
            cddbEmail = email
            emailHintShown = false
            edit.putString(PREF_CDDB_EMAIL, email)
        }
        edit.apply()
    }

    /** Connection test with the values in the fields, on [cddbTestWorker]. */
    private fun startCddbTest() {
        if (testingCddb || busy) return
        val server = selectedCddbServer()
        val email = editCddbEmail.text.toString().trim()
        val check = CddbSettings.check(server, email)
        val problem = cddbServerProblem(server, check)?.let { getString(R.string.cddb_server_url) + ": $it" }
            ?: cddbProblemText(check.email)?.let { "連絡先メールアドレス: $it" }
        if (problem != null) {
            textCddbTest.text = problem
            return
        }
        testingCddb = true
        buttonCddbTest.isEnabled = false
        textCddbTest.text = getString(R.string.cddb_testing)
        cddbTestWorker.execute {
            val text = try {
                describeCddbTest(CddbSettings.test(server, email, http))
            } catch (e: Exception) {
                "失敗: ${e.message}"
            }
            val ok = text.startsWith("接続できました")
            runOnUiThread {
                if (isDestroyed) return@runOnUiThread
                testingCddb = false
                buttonCddbTest.isEnabled = !busy
                textCddbTest.text = text
                // The settings work now: look the loaded disc up again if it was not found.
                val meta = metadata
                if (ok && !busy && cddbEnabled && toc != null && session != null && meta != null && !meta.found &&
                    server == cddbServer && email == cddbEmail
                ) lookUpAgain(0)
            }
        }
    }

    private fun describeCddbTest(r: CddbTestResult): String {
        if (!r.ok) {
            val hint = if (r.needsContactEmail) "\n" + getString(R.string.cddb_email_needed) else ""
            return "失敗: ${r.message}$hint"
        }
        var text = "接続できました: ${r.message}"
        if (r.detail.isNotEmpty()) text += " (${r.detail})"
        if (r.missingEmail) text += "\n連絡先メールアドレスが未設定です。gnudb は検索を拒否することがあります。"
        return text
    }

    /** After a lookup the server refused because of the greeting. */
    private fun showContactEmailNeeded() {
        if (emailHintShown || isFinishing) return
        emailHintShown = true
        AlertDialog.Builder(this)
            .setTitle("CDDB の連絡先メールアドレス")
            .setMessage(getString(R.string.cddb_email_needed) + "\n\n" + getString(R.string.cddb_email_note))
            .setPositiveButton("設定する") { _, _ ->
                showAdvanced(true)
                getSharedPreferences(PREFS, MODE_PRIVATE).edit().putBoolean(PREF_ADVANCED, true).apply()
                editCddbEmail.requestFocus()
            }
            .setNegativeButton("後で", null)
            .show()
    }

    private fun formatButtons(): List<Pair<String, RadioButton>> =
        listOf(
            "flac" to radioFlac, "oggflac" to radioOggFlac, "alac" to radioAlac, "wav" to radioWav, "opus" to radioOpus,
            "vorbis" to radioVorbis, "mka" to radioMka
        )

    private fun selectedFormat(): String = formatButtons().firstOrNull { it.second.isChecked }?.first ?: "flac"

    // .opus is not in the MIME type table of older Android versions, whose
    // document providers would then append ".ogg" to the name; a generic type
    // keeps the name as given.
    private fun mimeType(format: String): String = when (format) {
        "flac" -> "audio/flac"
        "alac" -> "audio/mp4"  // .m4a
        "vorbis", "oggflac" -> "audio/ogg"
        "opus" -> "application/octet-stream"
        "mka" -> "audio/x-matroska"
        else -> "audio/x-wav"
    }

    // C2 error pointers of the rip: whether they were used, then the tracks with C2 errors.
    private fun describeC2(status: C2Status, results: List<Pair<Int, RipResult>>): String {
        val lines = mutableListOf<String>()
        when (status) {
            C2Status.DISABLED -> lines += "C2: 使用しませんでした"
            C2Status.NOT_SUPPORTED -> lines += "C2: このドライブは C2 エラーポインタに対応していません"
            C2Status.USED, C2Status.GIVEN_UP -> {
                val errors = results.sumOf { it.second.c2ErrorSectors }
                val rereads = results.sumOf { it.second.c2Rereads }
                val suspicious = results.count { it.second.suspiciousSectors > 0 }
                lines += if (errors == 0) "C2: エラーなし"
                else "C2: エラー %d セクタ, 再読込 %d 回, 疑わしい位置のあるトラック %d".format(errors, rereads, suspicious)
                if (status == C2Status.GIVEN_UP)
                    lines += "C2: ドライブが C2 付きの読み取りを受け付けないため、途中から通常の読み取りにしました"
                for ((number, r) in results) {
                    if (r.c2ErrorSectors == 0) continue
                    lines += "Track %02d: C2 エラー %d セクタ / 再読込 %d 回 / 未解決 %d / 疑わしいセクタ %d".format(
                        number, r.c2ErrorSectors, r.c2Rereads, r.c2Unresolved, r.suspiciousSectors
                    )
                }
            }
        }
        return lines.joinToString("\n")
    }

    private fun selectedCacheMode(): CacheMode =
        CacheMode.entries.getOrElse(spinnerCache.selectedItemPosition) { CacheMode.AUTO }

    // Drive cache defeat (#34): what the test found, what the rip did before re-reads.
    private fun describeCache(status: CacheStatus, results: List<Pair<Int, RipResult>>): String {
        val size = if (status.cacheKb > 0) " (%d KB)".format(status.cacheKb) else ""
        val found = when (status.result) {
            CacheResult.NOT_TESTED -> ""
            CacheResult.NO_CACHE -> "音声データをキャッシュしないドライブです"
            CacheResult.FUA_WORKS -> "キャッシュあり、FUA が有効です"
            CacheResult.FUA_IGNORED -> "キャッシュあり、FUA は効きません"
            CacheResult.FUA_REJECTED -> "ドライブが FUA を受け付けません"
            CacheResult.UNKNOWN -> "判定できませんでした"
        }
        val method = when (status.method) {
            CacheMethod.NONE -> "対策なし"
            CacheMethod.FUA -> "FUA"
            CacheMethod.FLUSH -> "追い出し"
        }
        val defeats = results.sumOf { it.second.cacheDefeats }
        var line = "キャッシュ$size: " + (if (found.isEmpty()) "" else "$found → ") + method
        if (defeats > 0) line += ", 再読込前の対策 %d 回".format(defeats)
        if (status.fuaGivenUp) line += "\nキャッシュ: ドライブが FUA を受け付けないため、途中から追い出しにしました"
        return line
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
                    lines += "一致しません。読み取りオフセットを確認してください (詳細設定の「オフセットを自動検出」)"
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

    // WAV, FLAC and Ogg FLAC headers are patched at the end (and the other
    // Ogg pages are simply written in one go), so the file is written
    // locally and then streamed through the document's ParcelFileDescriptor
    // (providers may hand out non-seekable pipes).
    // A cancel during the copy (a disc image takes a while) or a failure
    // deletes the document again.
    private fun copyToDocument(source: File, document: Uri) {
        try {
            val pfd = contentResolver.openFileDescriptor(document, "w") ?: throw IOException("cannot open $document")
            pfd.use {
                FileOutputStream(it.fileDescriptor).use { out ->
                    FileInputStream(source).use { input ->
                        val buffer = ByteArray(1 shl 16)
                        while (true) {
                            if (cancelRequested) throw CancellationException()
                            val n = input.read(buffer)
                            if (n < 0) break
                            out.write(buffer, 0, n)
                        }
                    }
                }
            }
        } catch (e: Exception) {
            deleteDocument(document)
            throw e
        }
    }

    private fun deleteDocument(document: Uri) {
        try {
            DocumentsContract.deleteDocument(contentResolver, document)
        } catch (e: Exception) {
            // Best effort: the provider may not support deleting.
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
                meta.needsContactEmail -> "CDDB: 連絡先メールアドレスが必要です (${meta.message})  $audio"
                else -> "CDDB: 見つかりませんでした (${meta.message})  $audio"
            }
        )
        if (!meta.found && meta.needsContactEmail) showContactEmailNeeded()
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
        this.busy = busy
        buttonConnect.isEnabled = !busy
        buttonConnect.text = getString(if (connection == null) R.string.connect else R.string.reload)
        buttonFolder.isEnabled = !busy
        editOffset.isEnabled = !busy
        // While detecting it is the cancel button; not during a rip or without a disc.
        buttonDetectOffset.isEnabled = detecting || (!busy && toc != null && session != null)
        buttonDetectOffset.text = getString(if (detecting) R.string.detect_offset_cancel else R.string.detect_offset)
        checkVerify.isEnabled = !busy
        checkC2.isEnabled = !busy
        spinnerCache.isEnabled = !busy
        switchSingleFile.isEnabled = !busy
        checkCddb.isEnabled = !busy
        // CDDB settings (#38): not while ripping, detecting or looking up.
        spinnerCddbServer.isEnabled = !busy
        editCddbServer.isEnabled = !busy
        editCddbEmail.isEnabled = !busy
        buttonCddbTest.isEnabled = !busy && !testingCddb
        checkAccurateRip.isEnabled = !busy
        for ((_, radio) in formatButtons()) radio.isEnabled = !busy
        spinnerMatch.isEnabled = !busy
        listTracks.isEnabled = !busy
        updateRipButton(busy)
    }

    private fun updateRipButton(busy: Boolean = false) {
        buttonRip.isEnabled = ripping || (!busy && !detecting && toc != null && outputTree != null)
    }

    private fun showAdvanced(open: Boolean) {
        groupAdvanced.visibility = if (open) View.VISIBLE else View.GONE
        textAdvanced.text = getString(if (open) R.string.advanced_expanded else R.string.advanced_collapsed)
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
