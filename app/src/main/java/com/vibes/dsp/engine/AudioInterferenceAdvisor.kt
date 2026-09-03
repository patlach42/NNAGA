/*
 * Copyright (C) 2026 patlach42
 *
 * This file is part of NNAGA.
 *
 * NNAGA is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * NNAGA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with NNAGA. If not, see <https://www.gnu.org/licenses/>.
 */

package com.vibes.dsp.engine

import android.content.Context
import android.net.wifi.WifiManager
import android.os.PowerManager

/**
 * Conditions outside the app that are known to cost audio, and which the app
 * can see but not change.
 *
 * These are not guesses. Chasing an intermittent dropout on an Audient iD4
 * ended here: the signal leaving the driver was continuous and every driver
 * counter was zero, while a loopback showed the returned signal vanishing for
 * an instant every thirty seconds. Turning Wi-Fi off removed it completely -
 * three minutes clean against six expected events.
 *
 * An app cannot disable Wi-Fi or exempt itself from Doze on its own, so the
 * only honest thing to do is say so.
 */
object AudioInterferenceAdvisor {

    enum class Kind { WifiActive, BatteryOptimised }

    data class Advice(val kind: Kind, val message: String)

    /**
     * What is currently working against low-latency audio. Empty when nothing
     * known is. Cheap enough to call when a session starts or a screen opens;
     * not for the audio thread.
     */
    fun inspect(context: Context): List<Advice> {
        val advice = mutableListOf<Advice>()

        // Measured, not assumed: background Wi-Fi activity produced a dropout
        // every thirty seconds on the reference device, and no software
        // counter saw it because the fault was not in the data path.
        // Not fail-open: without ACCESS_WIFI_STATE this throws, and swallowing
        // that would report "nothing in the way" on a device where Wi-Fi is
        // interrupting audio. An unknown state is reported as unknown.
        val wifiEnabled: Boolean? = runCatching {
            context.applicationContext
                .getSystemService(WifiManager::class.java)?.isWifiEnabled
        }.getOrNull()
        if (wifiEnabled == null) {
            advice += Advice(
                Kind.WifiActive,
                "Wi-Fi state could not be read, so interference from it cannot " +
                    "be ruled out.",
            )
        }
        runCatching {
            if (wifiEnabled == true) {
                advice += Advice(
                    Kind.WifiActive,
                    "Wi-Fi is on. Its periodic background activity has been " +
                        "measured to interrupt USB audio about every thirty " +
                        "seconds. Turn Wi-Fi off while recording.",
                )
            }
        }

        // Doze and background restrictions can throttle the process between
        // callbacks. The exemption needs one tap from the user; it cannot be
        // granted from code.
        runCatching {
            val power = context.getSystemService(PowerManager::class.java)
            val packageName = context.packageName
            if (power != null && !power.isIgnoringBatteryOptimizations(packageName)) {
                advice += Advice(
                    Kind.BatteryOptimised,
                    "Battery optimisation is active for this app, so Doze " +
                        "and App Standby may restrict it in the background. " +
                        "Exempting it lifts those restrictions. It does not " +
                        "fix CPU frequency or thermal policy.",
                )
            }
        }

        return advice
    }

    /** True when nothing known to cost audio is currently in the way. */
    fun isClear(context: Context): Boolean = inspect(context).isEmpty()

    /**
     * Opens the system dialog asking to exempt this app from battery
     * optimisation. The user decides; an app cannot grant this to itself.
     *
     * Returns false when the dialog could not be shown, including when the
     * exemption is already in place, so a caller can fall back to the general
     * battery settings screen rather than appear to do nothing.
     */
    fun requestBatteryOptimisationExemption(context: Context): Boolean {
        val power = context.getSystemService(PowerManager::class.java)
            ?: return false
        if (power.isIgnoringBatteryOptimizations(context.packageName)) return false
        return runCatching {
            val intent = android.content.Intent(
                android.provider.Settings
                    .ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                android.net.Uri.parse("package:" + context.packageName),
            ).addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK)
            context.startActivity(intent)
            true
        }.getOrDefault(false)
    }
}
