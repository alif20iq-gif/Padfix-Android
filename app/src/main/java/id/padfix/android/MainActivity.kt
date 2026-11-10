package id.padfix.android

import android.app.Activity
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Bundle
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.webkit.WebChromeClient
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import rikka.shizuku.Shizuku
import java.io.File
import kotlin.concurrent.thread

class MainActivity : Activity() {
    private lateinit var status: TextView
    private lateinit var web: WebView
    @Volatile private var starting = false
    @Volatile private var dashboardLoaded = false

    private val permissionListener = Shizuku.OnRequestPermissionResultListener { requestCode, grantResult ->
        if (requestCode == REQUEST_CODE) {
            if (grantResult == PackageManager.PERMISSION_GRANTED) startBackend()
            else setStatus("Izin Shizuku ditolak. Berikan izin agar Padfix dapat dijalankan.")
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
            text = "Memeriksa Padfix dan Shizuku…"
            setTextColor(Color.WHITE); textSize = 14f; setPadding(8, 8, 8, 12)
        }
        val controls = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        val start = Button(this).apply {
            text = "Periksa / Mulai Padfix"
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
        // Coba pulihkan backend otomatis setiap aplikasi dibuka kembali.
        if (Shizuku.pingBinder()) {
            if (Shizuku.checkSelfPermission() == PackageManager.PERMISSION_GRANTED) startBackend()
            else {
                setStatus("Meminta izin Shizuku untuk memeriksa Padfix…")
                Shizuku.requestPermission(REQUEST_CODE)
            }
        } else setStatus("Shizuku belum aktif. Aktifkan Shizuku, lalu tekan Periksa / Mulai Padfix.")
    }

    private fun requestShizukuAndStart() {
        if (!Shizuku.pingBinder()) {
            setStatus("Shizuku belum aktif. Jalankan Shizuku terlebih dahulu."); return
        }
        if (Shizuku.checkSelfPermission() == PackageManager.PERMISSION_GRANTED) startBackend()
        else Shizuku.requestPermission(REQUEST_CODE)
    }

    private fun startBackend() {
        if (starting) return
        starting = true
        setStatus("Memeriksa backend Padfix…")
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
                "if ! pidof padfix >/dev/null 2>&1; then " +
                "rm -f /data/local/tmp/padfix.log; " +
                "nohup /data/local/tmp/padfix -w /data/local/tmp/padfix_index.html " +
                "</dev/null >/data/local/tmp/padfix.log 2>&1 & fi; " +
                "sleep 1; cat /data/local/tmp/padfix.log 2>/dev/null"
            val process = invokeShizukuProcess(arrayOf("sh", "-c", command))
            thread(name = "Padfix-bootstrap") {
                val output = try { process.inputStream.bufferedReader().use { it.readText() } }
                    catch (e: Exception) { "Gagal membaca status backend: ${e.message}" }
                try { process.waitFor() } catch (_: Exception) {}
                runOnUiThread {
                    starting = false
                    val url = Regex("http://127\\.0\\.0\\.1:8765/\\?t=[0-9a-fA-F]{16}").find(output)?.value
                    if (url != null) {
                        dashboardLoaded = true
                        web.loadUrl(url)
                        setStatus(if (output.contains("Konfigurasi terakhir dipulihkan"))
                            "Padfix aktif — konfigurasi terakhir dipulihkan."
                            else "Padfix aktif / sedang diperiksa.")
                    } else {
                        setStatus(output.lines().lastOrNull { it.isNotBlank() }
                            ?: "Backend tidak merespons. Periksa izin Shizuku dan /dev/uinput.")
                    }
                }
            }
        } catch (e: Exception) {
            starting = false
            setStatus("Gagal memulai/memeriksa Padfix: ${e.message}")
        }
    }

    private fun stopBackend() {
        try { invokeShizukuProcess(arrayOf("sh", "-c", "pkill -x padfix 2>/dev/null || true")) }
        catch (e: Exception) { setStatus("Gagal mengirim perintah Stop: ${e.message}"); return }
        web.loadUrl("about:blank")
        setStatus("Perintah berhenti dikirim.")
    }

    private fun invokeShizukuProcess(command: Array<String>): Process {
        val method = Shizuku::class.java.getDeclaredMethod(
            "newProcess", Array<String>::class.java, Array<String>::class.java, String::class.java
        )
        method.isAccessible = true
        return method.invoke(null, command, null, null) as Process
    }

    private fun shellQuote(value: String) = "'" + value.replace("'", "'\\''") + "'"
    private fun setStatus(message: String) { if (::status.isInitialized) status.text = message }

    // Tombol dari controller hanya untuk gamepad/Padfix, jangan biarkan WebView
    // atau Activity menafsirkannya sebagai navigasi/aksi keluar aplikasi.
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val gamepadSource = event.isFromSource(InputDevice.SOURCE_GAMEPAD) ||
            event.isFromSource(InputDevice.SOURCE_DPAD) ||
            event.isFromSource(InputDevice.SOURCE_JOYSTICK)
        if (gamepadSource) {
            when (event.keyCode) {
                KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B,
                KeyEvent.KEYCODE_BUTTON_C, KeyEvent.KEYCODE_BUTTON_X,
                KeyEvent.KEYCODE_BUTTON_Y, KeyEvent.KEYCODE_BUTTON_Z,
                KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1,
                KeyEvent.KEYCODE_BUTTON_L2, KeyEvent.KEYCODE_BUTTON_R2,
                KeyEvent.KEYCODE_BUTTON_THUMBL, KeyEvent.KEYCODE_BUTTON_THUMBR,
                KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_BUTTON_SELECT,
                KeyEvent.KEYCODE_BUTTON_MODE, KeyEvent.KEYCODE_DPAD_UP,
                KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_DPAD_LEFT,
                KeyEvent.KEYCODE_DPAD_RIGHT, KeyEvent.KEYCODE_DPAD_CENTER,
                KeyEvent.KEYCODE_HOME, KeyEvent.KEYCODE_BACK -> return true
            }
            // Key controller lain juga jangan diteruskan ke WebView.
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (event.action == MotionEvent.ACTION_MOVE && event.isFromSource(InputDevice.SOURCE_JOYSTICK)) return true
        return super.dispatchGenericMotionEvent(event)
    }

    override fun onDestroy() {
        Shizuku.removeRequestPermissionResultListener(permissionListener)
        super.onDestroy()
    }

    companion object { private const val REQUEST_CODE = 2907 }
}
