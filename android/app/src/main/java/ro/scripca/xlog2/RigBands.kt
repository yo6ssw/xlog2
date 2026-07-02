// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

package ro.scripca.xlog2

/**
 * A band the operator can QSY to from the rig-audio panel. [lowHz]..[highHz] is
 * the inclusive allocation (used to tell which band a frequency is on); [cwMidHz]
 * is the middle of the band's CW segment, used as the QSY target when no
 * last-used frequency has been recorded for the band yet.
 */
class RigBand(val name: String, val lowHz: Long, val highHz: Long, val cwMidHz: Long)

/** The HF/VHF bands offered by the rig-audio band selector, low to high. */
object RigBands {
    val all = listOf(
        RigBand("160m",   1_800_000,   2_000_000,   1_820_000),
        RigBand("80m",    3_500_000,   4_000_000,   3_535_000),
        RigBand("60m",    5_060_000,   5_450_000,   5_357_000),  // channelised; common CW/data channel
        RigBand("40m",    7_000_000,   7_300_000,   7_020_000),
        RigBand("30m",   10_100_000,  10_150_000,  10_115_000),
        RigBand("20m",   14_000_000,  14_350_000,  14_035_000),
        RigBand("17m",   18_068_000,  18_168_000,  18_081_000),
        RigBand("15m",   21_000_000,  21_450_000,  21_035_000),
        RigBand("12m",   24_890_000,  24_990_000,  24_902_000),
        RigBand("10m",   28_000_000,  29_700_000,  28_035_000),
        RigBand("6m",    50_000_000,  54_000_000,  50_050_000),
        RigBand("2m",   144_000_000, 148_000_000, 144_050_000),
    )

    /** The band a frequency falls in, or null if it's outside every allocation. */
    fun forHz(hz: Long): RigBand? = all.firstOrNull { hz in it.lowHz..it.highHz }
}
