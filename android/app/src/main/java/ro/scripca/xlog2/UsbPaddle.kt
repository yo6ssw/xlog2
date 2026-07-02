// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

package ro.scripca.xlog2

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import android.hardware.usb.UsbRequest
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.nio.ByteBuffer

/**
 * Reads the custom xlog2 vendor-HID Morse paddle (USB VID 0x1EAF, the companion
 * ../paddles firmware) over Android's USB Host API — the phone-side analogue of
 * the desktop's [HidPaddleInput] /dev/hidraw reader. Each interrupt report's byte 0
 * carries the contact state (bit0 = dit, bit1 = dah); edges drive [onDit]/[onDah]
 * (wired to the keyer). A worker thread auto-discovers the device, requests USB
 * permission once, and reads the interrupt endpoint (via the async [UsbRequest]
 * API — see [readDevice]) until unplugged/stopped.
 */
class UsbPaddle(context: Context) {

    var onDit: ((Boolean) -> Unit)? = null
    var onDah: ((Boolean) -> Unit)? = null

    private val _status = MutableStateFlow("USB paddle: idle")
    val status: StateFlow<String> = _status.asStateFlow()
    private val _connected = MutableStateFlow(false)
    val connected: StateFlow<Boolean> = _connected.asStateFlow()

    private val appCtx = context.applicationContext
    private val usb = appCtx.getSystemService(Context.USB_SERVICE) as UsbManager

    @Volatile private var running = false
    private var thread: Thread? = null

    fun start() {
        stop()
        running = true
        thread = Thread({ run() }, "xlog2-usbpaddle").apply { isDaemon = true; start() }
    }

    fun stop() {
        running = false
        thread?.let { it.interrupt(); it.join(1000) }
        thread = null
        _connected.value = false
    }

    private fun findDevice(): UsbDevice? =
        usb.deviceList.values.firstOrNull { it.vendorId == VID }

    private fun run() {
        var permissionRequested = false
        while (running) {
            val dev = findDevice()
            if (dev == null) {
                permissionRequested = false
                _status.value = "USB paddle: not found — plug it in (USB-C/OTG)"
                sleep(1500)
                continue
            }
            if (!usb.hasPermission(dev)) {
                if (!permissionRequested) {
                    requestPermission(dev)
                    permissionRequested = true
                    _status.value = "USB paddle: waiting for permission…"
                }
                sleep(1000)
                continue
            }
            permissionRequested = false
            readDevice(dev)   // blocks until disconnect / stop
        }
        _connected.value = false
    }

    /** Locate the HID interface's interrupt-IN endpoint and read its reports. */
    private fun readDevice(dev: UsbDevice) {
        var iface: UsbInterface? = null
        var epIn: UsbEndpoint? = null
        // The paddle enumerates as a composite device (CDC-ACM serial + HID). The
        // CDC comms interface (class 2) also has an interrupt-IN endpoint — its
        // modem-status notification channel — so we must match the *HID* interface
        // (class 3) specifically, not merely the first interrupt-IN endpoint, or we
        // end up reading the serial notification endpoint (which never carries
        // paddle reports) and spin re-claiming it.
        loop@ for (i in 0 until dev.interfaceCount) {
            val itf = dev.getInterface(i)
            for (e in 0 until itf.endpointCount) {
                val ep = itf.getEndpoint(e)
                if (itf.interfaceClass == UsbConstants.USB_CLASS_HID &&
                    ep.direction == UsbConstants.USB_DIR_IN &&
                    ep.type == UsbConstants.USB_ENDPOINT_XFER_INT
                ) {
                    iface = itf; epIn = ep; break@loop
                }
            }
        }
        if (iface == null || epIn == null) {
            _status.value = "USB paddle: no HID interrupt endpoint"
            sleep(2000)
            return
        }
        val conn: UsbDeviceConnection = usb.openDevice(dev) ?: run {
            _status.value = "USB paddle: open failed"
            sleep(2000)
            return
        }
        var lastDit = false
        var lastDah = false
        val req = UsbRequest()
        try {
            conn.claimInterface(iface, true)
            if (!req.initialize(conn, epIn)) {
                _status.value = "USB paddle: endpoint init failed"
                sleep(2000)
                return
            }
            _connected.value = true
            _status.value = "USB paddle: connected"
            // Interrupt IN via the async UsbRequest API. bulkTransfer() is
            // unreliable on interrupt endpoints — usbfs builds a *bulk* pipe, and
            // the kernel URB-type mismatch makes it return -1 on many devices
            // (silently swallowed as a timeout), so no edges ever arrive. Here the
            // firmware sends a report only on a contact change; the queued request
            // stays pending until then, so requestWait's 200 ms timeout is just how
            // often we re-check `running` while idle.
            val size = maxOf(2, epIn.maxPacketSize)
            val buf = ByteBuffer.allocate(size)
            if (!req.queue(buf)) {
                _status.value = "USB paddle: read queue failed"
                return
            }
            while (running) {
                // requestWait(timeout) signals an idle timeout by THROWING
                // TimeoutException (not returning null, as some docs imply) — the
                // request stays queued, so we just keep waiting and re-check
                // `running`. Letting it escape would tear down the whole session
                // every 200 ms (flapping the connected state + dropping held
                // contacts via the finally's safety key-up).
                val done = try {
                    conn.requestWait(200) ?: continue
                } catch (e: java.util.concurrent.TimeoutException) {
                    continue
                }
                if (done !== req) continue
                val n = buf.position()   // bytes transferred (single-arg queue, API 26+)
                if (n > 0) {
                    val b0 = buf.get(0).toInt() and 0xFF
                    val dit = (b0 and 0x01) != 0
                    val dah = (b0 and 0x02) != 0
                    if (dit != lastDit) { lastDit = dit; onDit?.invoke(dit) }
                    if (dah != lastDah) { lastDah = dah; onDah?.invoke(dah) }
                }
                buf.clear()
                if (!req.queue(buf)) {   // re-arm for the next report
                    _status.value = "USB paddle: read queue failed"
                    break
                }
            }
        } catch (e: Exception) {
            _status.value = "USB paddle: ${e.message ?: "read error"}"
        } finally {
            runCatching { req.cancel() }
            runCatching { req.close() }
            if (lastDit) onDit?.invoke(false)   // don't leave a contact stuck closed
            if (lastDah) onDah?.invoke(false)
            runCatching { conn.releaseInterface(iface) }
            conn.close()
            _connected.value = false
            if (running) _status.value = "USB paddle: disconnected — searching…"
        }
    }

    private fun requestPermission(dev: UsbDevice) {
        val pi = PendingIntent.getBroadcast(
            appCtx, 0,
            Intent(ACTION_USB_PERMISSION).setPackage(appCtx.packageName),
            PendingIntent.FLAG_MUTABLE,
        )
        runCatching { usb.requestPermission(dev, pi) }
    }

    private fun sleep(ms: Long) {
        try { Thread.sleep(ms) } catch (e: InterruptedException) { running = false }
    }

    companion object {
        private const val VID = 0x1EAF   // LeafLabs Maple (Blue Pill) — the paddle firmware
        private const val ACTION_USB_PERMISSION = "ro.scripca.xlog2.USB_PERMISSION"
    }
}
