package com.vibes.dsp.ui.live

import android.content.Context

object LiveLayoutPreferences {
    private const val PREFERENCES_NAME = "live_layout"
    private const val KEY_HORIZONTAL_PLUGINS = "horizontal_plugins"
    private const val KEY_TILE_ORDER = "tile_order"
    private const val KEY_VISIBLE_TILES = "visible_tiles"
    private const val KEY_INSPECTOR_TOGGLE_MIGRATED = "inspector_toggle_migrated"
    private const val KEY_FIT_TILES_ON_SCREEN = "fit_tiles_on_screen"
    private const val KEY_ARM_EXCLUSIVE_ON_TRACK_SELECTION = "arm_exclusive_on_track_selection"
    private const val KEY_HIDE_TRANSPORT_WITHOUT_LAUNCHER = "hide_transport_without_launcher"
    private const val TILE_HEIGHT_PREFIX = "tile_height_"
    private const val TRACK_COLOR_PREFIX = "track_color_"
    private const val MIN_UNKNOWN_TILE_HEIGHT = 96f
    private const val MAX_UNKNOWN_TILE_HEIGHT = 640f
    private const val DEFAULT_UNKNOWN_TILE_HEIGHT = 160f
    private const val DEFAULT_LAUNCHER_HEIGHT = 320f
    private const val DEFAULT_INSPECTOR_HEIGHT = 120f
    private const val DEFAULT_DEVICES_HEIGHT = 320f
    private const val DEFAULT_MIXER_HEIGHT = 176f
    private const val SEPARATOR = ","

    val canonicalTileIds: List<String> = listOf("launcher", "inspector", "devices", "mixer")

    fun getHorizontalPlugins(context: Context): Boolean =
        preferences(context).getBoolean(KEY_HORIZONTAL_PLUGINS, false)

    fun setHorizontalPlugins(context: Context, enabled: Boolean) {
        preferences(context).edit().putBoolean(KEY_HORIZONTAL_PLUGINS, enabled).apply()
    }

    fun getFitTilesOnScreen(context: Context): Boolean =
        preferences(context).getBoolean(KEY_FIT_TILES_ON_SCREEN, false)

    fun setFitTilesOnScreen(context: Context, enabled: Boolean) {
        preferences(context).edit().putBoolean(KEY_FIT_TILES_ON_SCREEN, enabled).apply()
    }

    fun getArmExclusiveOnTrackSelection(context: Context): Boolean =
        preferences(context).getBoolean(KEY_ARM_EXCLUSIVE_ON_TRACK_SELECTION, false)

    fun setArmExclusiveOnTrackSelection(context: Context, enabled: Boolean) {
        preferences(context).edit()
            .putBoolean(KEY_ARM_EXCLUSIVE_ON_TRACK_SELECTION, enabled)
            .apply()
    }

    fun getHideTransportWithoutLauncher(context: Context): Boolean =
        preferences(context).getBoolean(KEY_HIDE_TRANSPORT_WITHOUT_LAUNCHER, false)

    fun setHideTransportWithoutLauncher(context: Context, enabled: Boolean) {
        preferences(context).edit()
            .putBoolean(KEY_HIDE_TRANSPORT_WITHOUT_LAUNCHER, enabled)
            .apply()
    }


    fun getTrackColor(context: Context, trackId: Long, fallbackArgb: Int): Int {
        val fallback = opaqueArgb(fallbackArgb)
        val stored = preferences(context).getInt(TRACK_COLOR_PREFIX + trackId, fallback)
        return opaqueArgb(stored)
    }

    fun setTrackColor(context: Context, trackId: Long, argb: Int) {
        preferences(context).edit()
            .putInt(TRACK_COLOR_PREFIX + trackId, opaqueArgb(argb))
            .apply()
    }

    private fun opaqueArgb(argb: Int): Int = argb or 0xFF000000.toInt()


    fun getTileHeight(context: Context, tileId: String): Float {
        val key = TILE_HEIGHT_PREFIX + tileId
        val stored = preferences(context).getFloat(key, defaultTileHeight(tileId))
        return clampTileHeight(tileId, stored)
    }

    fun setTileHeight(context: Context, tileId: String, heightDp: Float) {
        preferences(context).edit()
            .putFloat(TILE_HEIGHT_PREFIX + tileId, clampTileHeight(tileId, heightDp))
            .apply()
    }

    fun defaultTileHeight(tileId: String): Float = when (tileId) {
        "launcher" -> DEFAULT_LAUNCHER_HEIGHT
        "inspector" -> DEFAULT_INSPECTOR_HEIGHT
        "devices" -> DEFAULT_DEVICES_HEIGHT
        "mixer" -> DEFAULT_MIXER_HEIGHT
        else -> DEFAULT_UNKNOWN_TILE_HEIGHT
    }

    fun clampTileHeight(tileId: String, heightDp: Float): Float {
        val (min, max) = when (tileId) {
            "launcher" -> 160f to 640f
            "inspector" -> 96f to 320f
            "devices" -> 120f to 640f
            "mixer" -> 120f to 400f
            else -> MIN_UNKNOWN_TILE_HEIGHT to MAX_UNKNOWN_TILE_HEIGHT
        }
        val value = if (heightDp.isNaN()) defaultTileHeight(tileId) else heightDp
        return value.coerceIn(min, max)
    }

    fun getTileOrder(context: Context): List<String> =
        normalizedOrder(preferences(context).getString(KEY_TILE_ORDER, null))

    fun setTileOrder(context: Context, order: List<String>) {
        preferences(context).edit()
            .putString(KEY_TILE_ORDER, normalizedOrder(order.joinToString(SEPARATOR)).joinToString(SEPARATOR))
            .apply()
    }

    fun getVisibleTiles(context: Context): Set<String> {
        val prefs = preferences(context)
        val stored = prefs.getString(KEY_VISIBLE_TILES, null)
            ?: return canonicalTileIds.toSet()
        val visible = stored.split(SEPARATOR)
            .filterTo(linkedSetOf()) { it in canonicalTileIds }
        if (!prefs.getBoolean(KEY_INSPECTOR_TOGGLE_MIGRATED, false)) {
            visible.add("inspector")
            prefs.edit()
                .putBoolean(KEY_INSPECTOR_TOGGLE_MIGRATED, true)
                .putString(KEY_VISIBLE_TILES, canonicalTileIds.filter { it in visible }.joinToString(SEPARATOR))
                .apply()
        }
        return visible
    }

    fun setVisibleTiles(context: Context, visibleTiles: Set<String>) {
        val normalized = canonicalTileIds.filter { it in visibleTiles }
        preferences(context).edit()
            .putBoolean(KEY_INSPECTOR_TOGGLE_MIGRATED, true)
            .putString(KEY_VISIBLE_TILES, normalized.joinToString(SEPARATOR))
            .apply()
    }

    private fun normalizedOrder(stored: String?): List<String> {
        val saved = stored.orEmpty().split(SEPARATOR).filter { it in canonicalTileIds }.distinct()
        return saved + canonicalTileIds.filterNot(saved::contains)
    }

    private fun preferences(context: Context) =
        context.applicationContext.getSharedPreferences(PREFERENCES_NAME, Context.MODE_PRIVATE)
}
