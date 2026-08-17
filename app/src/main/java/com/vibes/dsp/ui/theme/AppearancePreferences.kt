/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of NNAGA.
 */
package com.vibes.dsp.ui.theme

import android.content.Context
import android.content.SharedPreferences
import kotlin.math.pow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/** Persisted, app-wide appearance choices. */
object AppearancePreferences {
    data class Palette(val id: Int, val label: String, val argb: Int)

    /** Stable order is also the order shown in Interface settings. */
    val palettes: List<Palette> = listOf(
        Palette(0, "White", 0xFFFFFFFF.toInt()),
        Palette(1, "Amber", 0xFFFFC107.toInt()),
        Palette(2, "Orange", 0xFFFF9800.toInt()),
        Palette(3, "Red", 0xFFF44336.toInt()),
        Palette(4, "Pink", 0xFFE91E63.toInt()),
        Palette(5, "Violet", 0xFF9C27B0.toInt()),
        Palette(6, "Blue", 0xFF2196F3.toInt()),
        Palette(7, "Cyan", 0xFF00BCD4.toInt()),
        Palette(8, "Green", 0xFF4CAF50.toInt()),
        Palette(9, "Lime", 0xFFCDDC39.toInt())
    )

    /**
     * Returns the opaque black or white content color with the best WCAG contrast
     * against an opaque accent color.
     */
    fun contentArgbForAccent(accentArgb: Int): Int {
        val red = ((accentArgb ushr 16) and 0xFF) / 255f
        val green = ((accentArgb ushr 8) and 0xFF) / 255f
        val blue = (accentArgb and 0xFF) / 255f
        fun linearize(channel: Float): Float =
            if (channel <= 0.04045f) channel / 12.92f
            else ((channel + 0.055f) / 1.055f).pow(2.4f)
        val luminance = 0.2126f * linearize(red) +
            0.7152f * linearize(green) +
            0.0722f * linearize(blue)
        return if (luminance > 0.179f) 0xFF000000.toInt() else 0xFFFFFFFF.toInt()
    }

    private const val PREFERENCES_NAME = "appearance"
    private const val KEY_PALETTE_ID = "palette_id"
    private const val DEFAULT_PALETTE_ID = 0

    private val _accentArgb = MutableStateFlow(palette(DEFAULT_PALETTE_ID).argb)
    val accentArgb: StateFlow<Int> = _accentArgb.asStateFlow()
    private var registeredPreferences: SharedPreferences? = null
    private var listener: SharedPreferences.OnSharedPreferenceChangeListener? = null

    @Synchronized
    fun initialize(context: Context) {
        val preferences = context.applicationContext
            .getSharedPreferences(PREFERENCES_NAME, Context.MODE_PRIVATE)
        if (registeredPreferences === preferences) return
        listener?.let { registeredPreferences?.unregisterOnSharedPreferenceChangeListener(it) }
        registeredPreferences = preferences
        val preferenceListener = SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
            if (key == KEY_PALETTE_ID) _accentArgb.value = selectedPalette(preferences).argb
        }
        listener = preferenceListener
        preferences.registerOnSharedPreferenceChangeListener(preferenceListener)
        _accentArgb.value = selectedPalette(preferences).argb
    }

    fun setPalette(context: Context, paletteId: Int) {
        initialize(context)
        val safeId = palettes.firstOrNull { it.id == paletteId }?.id ?: DEFAULT_PALETTE_ID
        _accentArgb.value = palette(safeId).argb
        registeredPreferences?.edit()?.putInt(KEY_PALETTE_ID, safeId)?.apply()
    }

    fun selectedPaletteId(context: Context): Int {
        initialize(context)
        return selectedPalette(registeredPreferences ?: error("AppearancePreferences is not initialized")).id
    }

    fun palette(id: Int): Palette = palettes.firstOrNull { it.id == id } ?: palettes.first()

    private fun selectedPalette(preferences: SharedPreferences): Palette =
        palette(preferences.getInt(KEY_PALETTE_ID, DEFAULT_PALETTE_ID))
}
