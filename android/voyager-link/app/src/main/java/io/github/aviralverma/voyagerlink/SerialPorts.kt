package io.github.aviralverma.voyagerlink

import android.Manifest
import android.annotation.SuppressLint
import android.app.PendingIntent
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothSocket
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.hardware.usb.UsbManager
import android.os.Build
import com.hoho.android.usbserial.driver.UsbSerialDriver
import com.hoho.android.usbserial.driver.UsbSerialPort
import com.hoho.android.usbserial.driver.UsbSerialProber
import java.io.IOException
import java.util.UUID

/** One entry of the port list the device page shows (same fields as the Windows helper). */
data class PortInfo(
    val name: String,     // "usb:<device path>" or "bt:<address>": what the page sends back in "open"
    val desc: String,
    val likely: Boolean,  // probably a Voyager device: listed first by the page
    val usb: Boolean,
)

/** An open serial link to the device. write() and close() may be called from any thread. */
interface SerialTransport {
    @Throws(IOException::class)
    fun write(data: ByteArray)

    fun close()
}

/**
 * Lists and opens the two kinds of serial link Android offers: USB host serial
 * (OTG cable to the board's USB-UART bridge) and Bluetooth Classic serial (SPP)
 * to a paired device.
 */
class SerialPorts(private val context: Context) {

    companion object {
        const val USB_PREFIX = "usb:"
        const val BT_PREFIX = "bt:"
        const val ACTION_USB_PERMISSION = "io.github.aviralverma.voyagerlink.USB_PERMISSION"

        /** Standard Serial Port Profile UUID, which the ESP32's BluetoothSerial serves. */
        private val SPP_UUID: UUID = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB")
    }

    private val usbManager: UsbManager
        get() = context.getSystemService(Context.USB_SERVICE) as UsbManager

    private val btAdapter: BluetoothAdapter?
        get() = (context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter

    /** False on Android 12+ until the user has allowed "Nearby devices". */
    fun hasBluetoothPermission(): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return true
        return context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) ==
            PackageManager.PERMISSION_GRANTED
    }

    private fun usbDrivers(): List<UsbSerialDriver> =
        try {
            UsbSerialProber.getDefaultProber().findAllDrivers(usbManager)
        } catch (e: Exception) {
            emptyList()
        }

    @SuppressLint("MissingPermission")
    fun list(): List<PortInfo> {
        val out = ArrayList<PortInfo>()
        for (driver in usbDrivers()) {
            val device = driver.device
            val chip = driver.javaClass.simpleName.removeSuffix("SerialDriver")
            val product = try {
                device.productName
            } catch (e: Exception) {
                null
            }
            out.add(
                PortInfo(
                    name = USB_PREFIX + device.deviceName,
                    desc = "USB " + (product ?: chip),
                    likely = true,
                    usb = true,
                )
            )
        }
        val adapter = btAdapter
        if (adapter != null && adapter.isEnabled && hasBluetoothPermission()) {
            try {
                for (device in adapter.bondedDevices) {
                    val label = device.name ?: device.address
                    out.add(
                        PortInfo(
                            name = BT_PREFIX + device.address,
                            desc = "Bluetooth $label",
                            likely = label.startsWith("Voyager", ignoreCase = true),
                            usb = false,
                        )
                    )
                }
            } catch (e: SecurityException) {
                // Permission was revoked between the check and the call: list USB only.
            }
        }
        return out
    }

    /**
     * Opens a port from [list]. [onData] and [onLost] are called from a reader thread.
     * Throws IOException with a message meant for the user.
     */
    @Throws(IOException::class)
    fun open(
        name: String,
        baud: Int,
        onData: (ByteArray) -> Unit,
        onLost: (String) -> Unit,
    ): SerialTransport = when {
        name.startsWith(USB_PREFIX) -> openUsb(name.substring(USB_PREFIX.length), baud, onData, onLost)
        name.startsWith(BT_PREFIX) -> openBluetooth(name.substring(BT_PREFIX.length), onData, onLost)
        else -> throw IOException("unknown port $name")
    }

    // ------------------------------------------------------------------ USB

    @Throws(IOException::class)
    private fun openUsb(
        deviceName: String,
        baud: Int,
        onData: (ByteArray) -> Unit,
        onLost: (String) -> Unit,
    ): SerialTransport {
        val driver = usbDrivers().firstOrNull { it.device.deviceName == deviceName }
            ?: throw IOException("USB device not found. Plug it in again and refresh the list.")
        val device = driver.device
        if (!usbManager.hasPermission(device)) {
            // Android shows its own dialog; the page simply presses Connect again afterwards.
            val flags =
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) PendingIntent.FLAG_MUTABLE else 0
            val intent = Intent(ACTION_USB_PERMISSION).setPackage(context.packageName)
            usbManager.requestPermission(device, PendingIntent.getBroadcast(context, 0, intent, flags))
            throw IOException("Allow USB access in the dialog on the phone, then press Connect again.")
        }
        val connection = usbManager.openDevice(device)
            ?: throw IOException("Cannot open the USB device.")
        val port = driver.ports.firstOrNull() ?: run {
            connection.close()
            throw IOException("The USB device has no serial port.")
        }
        try {
            port.open(connection)
            port.setParameters(baud, 8, UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE)
            // DTR and RTS low: on ESP32 dev boards they drive EN / IO0 through the
            // auto-reset circuit, and raising them reboots the board mid-transfer.
            try {
                port.setDTR(false)
                port.setRTS(false)
            } catch (e: Exception) {
                // Not every bridge has these lines.
            }
        } catch (e: Exception) {
            try {
                port.close()
            } catch (ignored: Exception) {
            }
            throw IOException("Cannot open the USB serial port: ${e.message}")
        }
        return UsbTransport(port, onData, onLost)
    }

    private class UsbTransport(
        private val port: UsbSerialPort,
        private val onData: (ByteArray) -> Unit,
        private val onLost: (String) -> Unit,
    ) : SerialTransport {
        @Volatile
        private var running = true
        private val writeLock = Any()

        init {
            Thread({
                val buf = ByteArray(4096)
                try {
                    while (running) {
                        val n = port.read(buf, 200)
                        if (n > 0) onData(buf.copyOf(n))
                    }
                } catch (e: Exception) {
                    if (running) {
                        running = false
                        onLost("USB device disconnected")
                    }
                }
            }, "voyager-usb-read").start()
        }

        override fun write(data: ByteArray) {
            synchronized(writeLock) { port.write(data, 2000) }
        }

        override fun close() {
            running = false
            try {
                port.close()
            } catch (e: Exception) {
            }
        }
    }

    // ------------------------------------------------------------ Bluetooth

    @SuppressLint("MissingPermission")
    @Throws(IOException::class)
    private fun openBluetooth(
        address: String,
        onData: (ByteArray) -> Unit,
        onLost: (String) -> Unit,
    ): SerialTransport {
        val adapter = btAdapter ?: throw IOException("This phone has no Bluetooth.")
        if (!hasBluetoothPermission())
            throw IOException("Open the Voyager Link app and allow Bluetooth first.")
        if (!adapter.isEnabled) throw IOException("Bluetooth is switched off on the phone.")
        val socket: BluetoothSocket
        try {
            val device = adapter.getRemoteDevice(address)
            socket = device.createRfcommSocketToServiceRecord(SPP_UUID)
        } catch (e: Exception) {
            throw IOException("Cannot use that Bluetooth device: ${e.message}")
        }
        try {
            socket.connect() // blocks for up to about 12 s
        } catch (e: Exception) {
            try {
                socket.close()
            } catch (ignored: Exception) {
            }
            throw IOException(
                "Cannot connect over Bluetooth. Is Bluetooth serial switched on on the device " +
                    "(File Transfer page), and is the device in range?"
            )
        }
        return BluetoothTransport(socket, onData, onLost)
    }

    private class BluetoothTransport(
        private val socket: BluetoothSocket,
        private val onData: (ByteArray) -> Unit,
        private val onLost: (String) -> Unit,
    ) : SerialTransport {
        @Volatile
        private var running = true
        private val writeLock = Any()
        private val input = socket.inputStream
        private val output = socket.outputStream

        init {
            Thread({
                val buf = ByteArray(4096)
                try {
                    while (running) {
                        val n = input.read(buf)
                        if (n < 0) throw IOException("closed")
                        if (n > 0) onData(buf.copyOf(n))
                    }
                } catch (e: Exception) {
                    if (running) {
                        running = false
                        onLost("Bluetooth connection lost")
                    }
                }
            }, "voyager-bt-read").start()
        }

        override fun write(data: ByteArray) {
            synchronized(writeLock) {
                output.write(data)
                output.flush()
            }
        }

        override fun close() {
            running = false
            try {
                socket.close()
            } catch (e: Exception) {
            }
        }
    }
}
