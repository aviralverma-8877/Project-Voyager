# Voyager Link for Android

The Android counterpart of the Windows tray helper in `tools/voyager-link`. It lets
the Voyager device page, opened in the phone's browser, reach the device's serial
port for **File Transfer** (Kermit), over either:

- a **USB OTG cable** to the board's USB port (CP210x, CH34x, FTDI, CDC bridges), or
- **Bluetooth serial** (Bluetooth Classic SPP) to a paired device.

```
Voyager page in Chrome (http://192.168.4.1) ──ws://127.0.0.1:47321/serial──> Voyager Link app ──USB or Bluetooth──> ESP32
```

The app speaks the same local protocol as the Windows helper (see the header of
`HelperServer.kt`), so the page needs no Android-specific code.

## Using it

1. Install the APK and open the app. Allow notifications, and Bluetooth ("Nearby
   devices") if you want to use Bluetooth.
2. Tap **Start**. A notification stays up while the helper runs.
3. Connect the phone to the device's WiFi (or the same network) and tap **Open in
   browser**, or open the device page yourself.
4. On the page, open **File Transfer**. If Chrome asks to let the page access apps
   and services on this device, choose **Allow**.
5. In step 1 of the page, pick the `usb:` or `bt:` entry and press **Connect**.
   The first time you pick a USB device, Android asks for USB access: allow it and
   press **Connect** again.

### Bluetooth

1. On the device page, File Transfer, switch **Bluetooth serial** on. The device then
   shows up as `Voyager_XXXX` and keeps Bluetooth on across restarts.
2. Pair with it in Android's Bluetooth settings (the app has a shortcut).
3. Back on the page, press **Refresh ports** and pick the `bt:` entry.

The device uses Bluetooth while something is connected to it that way and USB
otherwise, so you can switch between the two by connecting through the other one.

## Building

```
cd android/voyager-link
gradle assembleDebug        # Gradle 8.7+ and JDK 17; no wrapper is checked in
```

The `Android app` GitHub Actions workflow runs the unit tests and attaches a debug
APK to each run. Debug APKs are signed with a throwaway key, so an APK from one run
cannot be installed over one from another run without uninstalling first. A release
build needs a signing key of your own.

## Security

- Listens on loopback only (`127.0.0.1`), never on the network.
- Only answers pages served from private, link-local or loopback IPs, `.local` names
  or `localhost` (`OriginRules.kt`, with unit tests). Any other website open in the
  browser is refused.
