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
import android.view.View
import android.view.WindowInsets
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.ListView
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.IOException
import java.util.concurrent.CancellationException
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * Single screen: connect to a USB CD drive, show its TOC, rip the selected
 * tracks to WAV files in a folder chosen with the Storage Access Framework.
 * USB I/O and ripping run on one worker thread ([worker]); the native
 * session is only touched from there (except cancel()).
 */
class MainActivity : Activity() {
    private companion object {
        const val ACTION_USB_PERMISSION = "io.github.noribow.cdreader.USB_PERMISSION"
        const val REQUEST_FOLDER = 1
        const val PREFS = "settings"
        const val PREF_FOLDER = "outputTree"
        const val PREF_OFFSET = "readOffset"
        const val MAX_RETRIES = 5
        const val READY_WAIT_SECONDS = 30
    }

    private lateinit var usbManager: UsbManager
    private val worker: ExecutorService = Executors.newSingleThreadExecutor()

    private lateinit var buttonConnect: Button
    private lateinit var buttonFolder: Button
    private lateinit var buttonRip: Button
    private lateinit var textDrive: TextView
    private lateinit var textDisc: TextView
    private lateinit var textFolder: TextView
    private lateinit var textStatus: TextView
    private lateinit var listTracks: ListView
    private lateinit var editOffset: EditText
    private lateinit var checkVerify: CheckBox
    private lateinit var progress: ProgressBar

    // Owned by the worker thread once opened.
    private var device: UsbDevice? = null
    private var connection: UsbDeviceConnection? = null
    private var massStorage: MassStorageInterface? = null
    @Volatile private var session: CdSession? = null
    private val sessionLock = Any()  // cancel() from the UI thread vs. close() on the worker

    // UI thread state.
    private var driveName = ""
    private var toc: DiscToc? = null
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
        listTracks = findViewById(R.id.listTracks)
        editOffset = findViewById(R.id.editOffset)
        checkVerify = findViewById(R.id.checkVerify)
        progress = findViewById(R.id.progress)
        applySystemBarInsets(findViewById(R.id.root))

        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        editOffset.setText(prefs.getInt(PREF_OFFSET, 0).toString())
        prefs.getString(PREF_FOLDER, null)?.let { setOutputTree(Uri.parse(it)) }

        buttonConnect.setOnClickListener { if (connection == null) connect() else reloadDisc() }
        buttonFolder.setOnClickListener {
            startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQUEST_FOLDER)
        }
        buttonRip.setOnClickListener { if (ripping) cancelRip() else startRip() }

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

    // Worker thread: waits for the disc to spin up, then reads the TOC.
    private fun loadDisc(s: CdSession) {
        runOnUiThread {
            toc = null
            listTracks.adapter = null
            textDisc.text = ""
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
    }

    private fun cancelRip() {
        synchronized(sessionLock) { session?.cancel() }
    }

    private fun closeDevice() {
        val conn = connection ?: return
        val ms = massStorage
        connection = null
        massStorage = null
        device = null
        toc = null
        listTracks.adapter = null
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
        val drive = driveName

        ripping = true
        setBusy(true)
        buttonRip.isEnabled = true
        buttonRip.text = getString(R.string.cancel)
        progress.progress = 0

        worker.execute {
            val temp = File(cacheDir, "rip.wav")
            val log = StringBuilder()
            log.append("cdreader (Android) rip log\n")
                .append("Drive: ").append(drive).append('\n')
                .append("CDDB disc id: ").append(disc.cddbHex).append('\n')
                .append("Read offset: ").append(offset).append(" samples\n")
                .append("Verify: ").append(if (verify) "yes" else "no").append("\n\n")
            var message: String
            try {
                val dir = createDocument(treeDocument(tree), DocumentsContract.Document.MIME_TYPE_DIR, "cd_${disc.cddbHex}")
                var problems = 0
                for ((index, track) in selected.withIndex()) {
                    val label = "トラック ${track.number} (${index + 1}/${selected.size})"
                    runOnUiThread { setStatus("$label を読み取り中…") }
                    val r = s.ripTrack(track.number, temp.path, offset, MAX_RETRIES, verify) { done, total ->
                        val permille = if (total > 0) (1000L * done / total).toInt() else 0
                        runOnUiThread {
                            progress.progress = permille
                            setStatus("$label  ${permille / 10}%")
                        }
                    }
                    val name = "Track%02d.wav".format(track.number)
                    copyToDocument(temp, createDocument(dir, "audio/x-wav", name))
                    if (!r.clean) problems++
                    log.append("Track %2d  %s  CRC32 %08X  retries %d  unreadable sectors %d  padded samples %d\n"
                        .format(track.number, name, r.crc32, r.retries, r.unreadableSectors, r.paddedSamples))
                }
                message = if (problems == 0) "完了しました (${selected.size} トラック)"
                else "完了しましたが、$problems トラックに読めないセクタがありました (rip.log を参照)"
                log.append('\n').append(if (problems == 0) "All tracks ripped without errors" else "Finished with errors").append('\n')
                writeText(createDocument(dir, "application/octet-stream", "rip.log"), log.toString())
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
            }
        }
    }

    private fun treeDocument(tree: Uri): Uri =
        DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree))

    private fun createDocument(parent: Uri, mimeType: String, name: String): Uri =
        DocumentsContract.createDocument(contentResolver, parent, mimeType, name)
            ?: throw IOException("$name を作成できませんでした")

    // The WAV header is patched at the end, so the file is written locally and
    // then streamed through the document's ParcelFileDescriptor (providers
    // may hand out non-seekable pipes).
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
        val audio = disc.tracks.count { it.isAudio }
        textDisc.text = "%d トラック  全長 %s  CDDB %s".format(
            disc.tracks.size, formatMsf(disc.leadOutLba), disc.cddbHex
        )
        val rows = disc.tracks.map { t ->
            val kind = if (t.isAudio) "" else "  (データ)"
            val emphasis = if (t.preEmphasis) "  (プリエンファシス)" else ""
            "%02d   %s%s%s".format(t.number, formatMsf(t.lengthSectors), kind, emphasis)
        }
        listTracks.adapter = ArrayAdapter(this, android.R.layout.simple_list_item_multiple_choice, rows)
        disc.tracks.forEachIndexed { i, t -> listTracks.setItemChecked(i, t.isAudio) }
        setStatus("$audio 個のオーディオトラック")
        setBusy(false)
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
