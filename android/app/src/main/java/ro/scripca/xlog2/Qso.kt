package ro.scripca.xlog2

/**
 * A logged contact, mirroring the C++ Qso fields the UI touches. The wire layout
 * (US-separated, fixed order) is byte-identical to qsoToWire()/wireToForm() in
 * xlog_jni.cpp — index 0 is the stored id, then the entry-form fields, then the
 * derived DXCC fields. The DXCC fields are ignored on input (the core re-derives
 * them from the callsign), but populated on output for display.
 */
data class Qso(
    val id: Long = 0,
    val date: String = "",
    val timeOn: String = "",
    val timeOff: String = "",
    val call: String = "",
    val band: String = "",
    val mode: String = "",
    val freq: String = "",
    val rstSent: String = "599",
    val rstRcvd: String = "599",
    val name: String = "",
    val qth: String = "",
    val locator: String = "",
    val power: String = "",
    val qslSent: Boolean = false,
    val qslRcvd: Boolean = false,
    val comment: String = "",
    val country: String = "",
    val cqZone: String = "",
    val ituZone: String = "",
    val continent: String = "",
) {
    fun toWire(): String {
        val us = XlogCore.US
        return buildString {
            append(id); append(us)
            append(date); append(us)
            append(timeOn); append(us)
            append(timeOff); append(us)
            append(call); append(us)
            append(band); append(us)
            append(mode); append(us)
            append(freq); append(us)
            append(rstSent); append(us)
            append(rstRcvd); append(us)
            append(name); append(us)
            append(qth); append(us)
            append(locator); append(us)
            append(power); append(us)
            append(if (qslSent) "Y" else "N"); append(us)
            append(if (qslRcvd) "Y" else "N"); append(us)
            append(comment); append(us)
            append(country); append(us)
            append(cqZone); append(us)
            append(ituZone); append(us)
            append(continent)
        }
    }

    companion object {
        fun fromWire(wire: String): Qso {
            val f = wire.split(XlogCore.US)
            fun at(i: Int) = f.getOrElse(i) { "" }
            return Qso(
                id = at(0).toLongOrNull() ?: 0,
                date = at(1), timeOn = at(2), timeOff = at(3), call = at(4),
                band = at(5), mode = at(6), freq = at(7), rstSent = at(8),
                rstRcvd = at(9), name = at(10), qth = at(11), locator = at(12),
                power = at(13), qslSent = at(14) == "Y", qslRcvd = at(15) == "Y",
                comment = at(16), country = at(17), cqZone = at(18),
                ituZone = at(19), continent = at(20),
            )
        }
    }
}
