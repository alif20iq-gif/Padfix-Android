package id.padfix.android

import android.app.Activity
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Bundle
import android.view.InputDevice
import android.view.MotionEvent
import android.view.View
import android.webkit.WebChromeClient
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import rikka.shizuku.Shizuku
import java.io.File
import kotlin.concurrent.thread

class MainActivity : Activity() {
    private lateinit var status: TextView
    private lateinit var web: WebView
    private var backend: Process? = null
    private var dashboardLoaded = false

    private val permissionListener = Shizuku.OnRequestPermissionResultListener { requestCode, grantResult ->
        if (requestCode == REQUEST_CODE) {
            if (grantResult == PackageManager.PERMISSION_GRANTED) startBackend()
            else setStatus("Izin Shizuku ditolak.")
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.statusBarColor = Color.rgb(11, 16, 32)
        window.navigationBarColor = Color.rgb(11, 16, 32)
        Shizuku.addRequestPermissionResultListener(permissionListener)

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.rgb(11, 16, 32))
            setPadding(12, 12, 12, 12)
        }
        status = TextView(this).apply {
            text = "Padfix siap. Hubungkan controller dan aktifkan Shizuku."
            setTextColor(Color.WHITE)
            textSize = 14f
            setPadding(8, 8, 8, 12)
        }
        val controls = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        val start = Button(this).apply {
            text = "Mulai Padfix"
            setOnClickListener { requestShizukuAndStart() }
        }
        val stop = Button(this).apply {
            text = "Stop"
            setOnClickListener { stopBackend() }
        }
        controls.addView(start, LinearLayout.LayoutParams(0, -2, 1f))
        controls.addView(stop, LinearLayout.LayoutParams(0, -2, 1f))
        web = WebView(this).apply {
            setBackgroundColor(Color.rgb(11, 16, 32))
            settings.javaScriptEnabled = true
            settings.domStorageEnabled = true
            settings.loadsImagesAutomatically = true
            webViewClient = WebViewClient()
            webChromeClient = WebChromeClient()
        }
        root.addView(status)
        root.addView(controls)
        root.addView(web, LinearLayout.LayoutParams(-1, 0, 1f))
        setContentView(root)
    }

    private fun requestShizukuAndStart() {
        if (!Shizuku.pingBinder()) {
            setStatus("Shizuku belum aktif. Jalankan Shizuku terlebih dahulu.")
            return
        }
        if (Shizuku.checkSelfPermission() == PackageManager.PERMISSION_GRANTED) {
            startBackend()
        } else {
            Shizuku.requestPermission(REQUEST_CODE)
        }
    }

    private fun startBackend() {
        if (backend != null) {
            setStatus("Padfix sedang berjalan.")
            return
        }
        try {
            val dir = getExternalFilesDir(null) ?: error("Penyimpanan aplikasi tidak tersedia")
            val bin = File(dir, "padfix")
            val html = File(dir, "index.html")
            assets.open("padfix").use { input -> bin.outputStream().use { input.copyTo(it) } }
            assets.open("index.html").use { input -> html.outputStream().use { input.copyTo(it) } }
            val command = "mkdir -p /data/local/tmp && " +
                "cp ${shellQuote(bin.absolutePath)} /data/local/tmp/padfix && " +
                "cp ${shellQuote(html.absolutePath)} /data/local/tmp/padfix_index.html && " +
                "chmod 755 /data/local/tmp/padfix && " +
                "exec /data/local/tmp/padfix -w /data/local/tmp/padfix_index.html"
            backend = Shizuku.newProcess(arrayOf("sh", "-c", command), null, null)
            dashboardLoaded = false
            setStatus("Backend berjalan. Menunggu alamat dashboard…")
            val runningProcess = backend!!
            thread(name = "Padfix-log-reader") {
                try {
                    runningProcess.inputStream.bufferedReader().useLines { lines ->
                        lines.forEach { line ->
                            runOnUiThread { setStatus(line) }
                            val url = Regex("http://127\\.0\\.0\\.1:8765/\\?t=[0-9a-fA-F]{16}").find(line)?.value
                            if (url != null && !dashboardLoaded) {
                                dashboardLoaded = true
                                runOnUiThread { web.loadUrl(url) }
                            }
                        }
                    }
                } catch (e: Exception) {
                    runOnUiThread { setStatus("Log backend: ${e.message}") }
                }
                runOnUiThread {
                    if (backend === runningProcess) {
                        backend = null
                        setStatus("Backend berhenti. Periksa izin /dev/input dan /dev/uinput.")
                    }
                }
            }
        } catch (e: Exception) {
            backend = null
            setStatus("Gagal memulai: ${e.message}")
        }
    }

    private fun stopBackend() {
        try {
            Shizuku.newProcess(arrayOf("sh", "-c", "pkill -x padfix 2>/dev/null || true"), null, null)
            backend?.destroy()
        } catch (_: Exception) {
        }
        backend = null
        web.loadUrl("about:blank")
        setStatus("Perintah berhenti dikirim.")
    }

    private fun shellQuote(value: String): String = "'" + value.replace("'", "'\\''") + "'"

    private fun setStatus(message: String) {
        if (::status.isInitialized) status.text = message
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (event.action == MotionEvent.ACTION_MOVE &&
            event.isFromSource(InputDevice.SOURCE_JOYSTICK)) {
            // Jangan biarkan gerakan analog fisik menjadi scroll pada WebView.
            // Backend shell tetap membaca event asli melalui evdev.
            return true
        }
        return super.dispatchGenericMotionEvent(event)
    }

    override fun onDestroy() {
        Shizuku.removeRequestPermissionResultListener(permissionListener)
        super.onDestroy()
    }

    companion object {
        private const val REQUEST_CODE = 2907
    }
}
