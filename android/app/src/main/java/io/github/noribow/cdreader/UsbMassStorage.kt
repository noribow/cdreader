package io.github.noribow.cdreader

import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbInterface

/** The Bulk-Only Transport interface of a USB mass storage device and its two bulk endpoints. */
data class MassStorageInterface(
    val usbInterface: UsbInterface,
    val bulkIn: UsbEndpoint,
    val bulkOut: UsbEndpoint,
)

private const val PROTOCOL_BULK_ONLY = 0x50

// SCSI transparent command set, MMC-5 (ATAPI), SFF-8070i: what CD drives report.
private val CD_SUBCLASSES = setOf(0x06, 0x02, 0x05)

fun findMassStorageInterface(device: UsbDevice): MassStorageInterface? {
    for (i in 0 until device.interfaceCount) {
        val intf = device.getInterface(i)
        if (intf.interfaceClass != UsbConstants.USB_CLASS_MASS_STORAGE) continue
        if (intf.interfaceSubclass !in CD_SUBCLASSES || intf.interfaceProtocol != PROTOCOL_BULK_ONLY) continue
        var bulkIn: UsbEndpoint? = null
        var bulkOut: UsbEndpoint? = null
        for (e in 0 until intf.endpointCount) {
            val ep = intf.getEndpoint(e)
            if (ep.type != UsbConstants.USB_ENDPOINT_XFER_BULK) continue
            if (ep.direction == UsbConstants.USB_DIR_IN) bulkIn = bulkIn ?: ep else bulkOut = bulkOut ?: ep
        }
        if (bulkIn != null && bulkOut != null) return MassStorageInterface(intf, bulkIn, bulkOut)
    }
    return null
}

fun describeDevice(device: UsbDevice): String {
    val name = listOfNotNull(device.manufacturerName, device.productName).joinToString(" ")
    return name.ifBlank { "USB %04x:%04x".format(device.vendorId, device.productId) }
}
