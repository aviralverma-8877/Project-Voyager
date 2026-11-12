package io.github.aviralverma.voyagerlink

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import android.widget.Toast

/**
 * The only screen: start / stop the helper, see which devices the phone can use,
 * allow Bluetooth, and open the device page in the browser.
 */
class MainActivity : Activity() {

    companion object {
        private const val PREFS = "voyager"
        private const val KEY_URL = "url"
        private const val DEFAULT_URL = "http://192.168.4.1" // the device's own access point
        private const val REQUEST_PERMISSIONS = 1
    }

    private lateinit var status: TextView
    private lateinit var toggle: Button
    private lateinit var portsView: TextView
    private lateinit var btPermission: Button
    private lateinit var url: EditText
    private lateinit var serialPorts: SerialPorts

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        serialPorts = SerialPorts(applicationContext)

        status = findViewById(R.id.status)
        toggle = findViewById(R.id.toggle)
        portsView = findViewById(R.id.ports)
        btPermission = findViewById(R.id.bt_permission)
        url = findViewById(R.id.url)

        url.setText(getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(KEY_URL, DEFAULT_URL))

        toggle.setOnClickListener {
            if (HelperState.running) {
                stopService(Intent(this, HelperService::class.java))
            } else {
                startForegroundService(Intent(this, HelperService::class.java))
            }
        }
        findViewById<Button>(R.id.refresh).setOnClickListener { refreshPorts() }
        btPermission.setOnClickListener { requestRuntimePermissions() }
        findViewById<Button>(R.id.bt_settings).setOnClickListener {
            try {
                startActivity(Intent(Settings.ACTION_BLUETOOTH_SETTINGS))
            } catch (e: Exception) {
                Toast.makeText(this, "Open Bluetooth settings from the system Settings app.", Toast.LENGTH_LONG).show()
            }
        }
        findViewById<Button>(R.id.open_page).setOnClickListener { openPage() }

        requestRuntimePermissions()
    }

    override fun onResume() {
        super.onResume()
        HelperState.onChange = { runOnUiThread { render() } }
        render()
        refreshPorts()
    }

    override fun onPause() {
        HelperState.onChange = null
        super.onPause()
    }

    /** Android 12+: Bluetooth needs "Nearby devices". Android 13+: notifications need consent. */
    private fun requestRuntimePermissions() {
        val wanted = ArrayList<String>()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S && !serialPorts.hasBluetoothPermission())
            wanted.add(Manifest.permission.BLUETOOTH_CONNECT)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
            wanted.add(Manifest.permission.POST_NOTIFICATIONS)
        if (wanted.isNotEmpty()) requestPermissions(wanted.toTypedArray(), REQUEST_PERMISSIONS)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        render()
        refreshPorts()
    }

    private fun render() {
        val s = HelperState
        toggle.setText(if (s.running) R.string.stop else R.string.start)
        val open = s.openPort
        status.text = when {
            s.error != null && !s.running -> s.error
            !s.running -> getString(R.string.status_stopped)
            open != null -> "Running. Connected to $open."
            s.pages > 0 -> "Running. The device page is connected; pick a port there."
            else -> "Running on port ${s.port}. Open the device page and go to File Transfer."
        }
        btPermission.visibility = if (serialPorts.hasBluetoothPermission()) View.GONE else View.VISIBLE
    }

    private fun refreshPorts() {
        val list = serialPorts.list()
        portsView.text =
            if (list.isEmpty()) getString(R.string.no_ports)
            else list.joinToString("\n") { "• " + it.desc }
    }

    private fun openPage() {
        var address = url.text.toString().trim()
        if (address.isEmpty()) address = DEFAULT_URL
        if (!address.startsWith("http://") && !address.startsWith("https://")) address = "http://$address"
        getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putString(KEY_URL, address).apply()
        if (!HelperState.running) startForegroundService(Intent(this, HelperService::class.java))
        try {
            startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(address)))
        } catch (e: Exception) {
            Toast.makeText(this, "No browser found to open $address", Toast.LENGTH_LONG).show()
        }
    }
}
