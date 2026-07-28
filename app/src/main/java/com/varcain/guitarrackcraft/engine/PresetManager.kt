/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of Guitar RackCraft.
 *
 * Guitar RackCraft is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Guitar RackCraft is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Guitar RackCraft. If not, see <https://www.gnu.org/licenses/>.
 */

package com.varcain.guitarrackcraft.engine

import android.content.Context
import android.util.Base64
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/**
 * Manages versioned rack presets stored in filesDir/presets/.
 * Presets contain graph controls and plugin state; WAV/transport data is transient.
 */
class PresetManager(private val engine: NativeEngine) {

    companion object {
        private const val TAG = "PresetManager"
        private const val PRESETS_DIR = "presets"
    }

    private fun presetsDir(context: Context): File {
        val dir = File(context.filesDir, PRESETS_DIR)
        if (!dir.exists()) dir.mkdirs()
        return dir
    }

    /**
     * Save the current chain state as a named preset.
     * @return true if saved successfully.
     */
    fun savePreset(context: Context, name: String): Boolean {
        val stateJson = engine.saveRackState()
        if (stateJson == null) {
            Log.e(TAG, "savePreset: nativeSaveRackState returned null")
            return false
        }

        return try {
            val root = JSONObject(stateJson)
            if (root.optInt("version", -1) != 2 ||
                !root.has("tracks") || !root.has("master")) return false
            root.put("presetName", name)
            root.put("timestamp", System.currentTimeMillis())

            // Flatten track + master URIs for quick identification.
            val uris = JSONArray()
            root.optJSONArray("tracks")?.let { tracks ->
                for (i in 0 until tracks.length()) {
                    tracks.optJSONObject(i)?.optJSONArray("plugins")?.let { plugins ->
                        for (j in 0 until plugins.length()) uris.put(plugins.optJSONObject(j)?.optString("uri", "") ?: "")
                    }
                }
            }
            root.optJSONObject("master")?.optJSONArray("plugins")?.let { plugins ->
                for (j in 0 until plugins.length()) uris.put(plugins.optJSONObject(j)?.optString("uri", "") ?: "")
            }
            root.put("pluginUris", uris)

            val file = File(presetsDir(context), "$name.json")
            file.writeText(root.toString(2))
            Log.i(TAG, "savePreset: saved '$name' to ${file.absolutePath}")
            true
        } catch (e: Exception) {
            Log.e(TAG, "savePreset: invalid native state", e)
            false
        }

    }

    fun loadPreset(context: Context, name: String): Boolean {
        val file = File(presetsDir(context), "$name.json")
        if (!file.exists()) {
            Log.e(TAG, "loadPreset: file not found: ${file.absolutePath}")
            return false
        }
        return loadPresetFromJson(file.readText())
    }

    fun loadPresetFromJson(json: String): Boolean {
        val root = try { JSONObject(json) } catch (e: Exception) {
            Log.e(TAG, "loadPresetFromJson: malformed JSON", e); return false
        }
        val version = if (root.has("version")) root.optInt("version", -1) else 1
        val trackObjects = ArrayList<JSONObject>()
        val masterObject: JSONObject
        when (version) {
            1 -> {
                val plugins = root.optJSONArray("plugins") ?: return false
                trackObjects += JSONObject().put("volume", 1.0).put("inputArmed", true).put("plugins", plugins)
                masterObject = JSONObject().put("plugins", JSONArray())
            }
            2 -> {
                val tracks = root.optJSONArray("tracks") ?: return false
                if (tracks.length() == 0) return false
                for (i in 0 until tracks.length()) {
                    val t = tracks.optJSONObject(i) ?: return false
                    if (t.optJSONArray("plugins") == null) return false
                    trackObjects += t
                }
                masterObject = root.optJSONObject("master") ?: return false
                if (masterObject.optJSONArray("plugins") == null) return false
            }
            else -> return false
        }
        // Validate every plugin entry completely before touching the current graph.
        fun validatePlugins(plugins: JSONArray): Boolean {
            for (i in 0 until plugins.length()) {
                val p = plugins.optJSONObject(i) ?: return false
                if (p.optString("uri", "").isEmpty()) return false
                if (p.optJSONArray("controlPorts") == null || p.optJSONArray("stateProperties") == null) return false
            }
            return true
        }
        if (trackObjects.any { !validatePlugins(it.getJSONArray("plugins")) } ||
            !validatePlugins(masterObject.getJSONArray("plugins"))) return false

        if (!engine.clearTrackWavs()) return false
        val existing = engine.getTracks().toMutableList()
        while (existing.size < trackObjects.size) {
            val id = engine.addTrack()
            if (id == 0L) return false
            existing += engine.getTracks().last()
        }
        while (existing.size > trackObjects.size && existing.size > 1) {
            if (!engine.removeTrack(existing.removeLast().id)) return false
        }
        fun restorePath(pathId: Long, obj: JSONObject, controls: Boolean): Boolean {
            val size = engine.getRackSize(pathId)
            for (i in size - 1 downTo 0) if (!engine.removePluginFromRack(pathId, i)) return false
            if (controls) {
                val idx = existing.indexOfFirst { it.id == pathId }
                if (idx >= 0) {
                    val t = trackObjects[idx]
                    if (!engine.setTrackVolume(pathId, t.optDouble("volume", 1.0).toFloat()) ||
                        !engine.setTrackInputArmed(pathId, t.optBoolean("inputArmed", false))) return false
                }
            }
            val plugins = obj.getJSONArray("plugins")
            for (i in 0 until plugins.length()) {
                val p = plugins.getJSONObject(i)
                val pos = engine.addPluginToRack(pathId, "${p.optString("format", "LV2")}:${p.getString("uri")}", -1)
                if (pos < 0 || !restorePluginFromJson(pathId, pos, p)) return false
            }
            return true
        }
        for (i in trackObjects.indices) if (!restorePath(existing[i].id, trackObjects[i], true)) return false
        return restorePath(0L, masterObject, false)
    }

    /**
     * List all saved presets.
     * @return list of preset names (without .json extension).
     */
    fun listPresets(context: Context): List<String> {
        val dir = presetsDir(context)
        return dir.listFiles { f -> f.extension == "json" }
            ?.map { it.nameWithoutExtension }
            ?.sorted()
            ?: emptyList()
    }

    /**
     * Delete a saved preset.
     * @return true if deleted.
     */
    fun deletePreset(context: Context, name: String): Boolean {
        val file = File(presetsDir(context), "$name.json")
        val ok = file.delete()
        Log.i(TAG, "deletePreset: '$name' deleted=$ok")
        return ok
    }

    fun getPresetJson(context: Context, name: String): String? {
        val file = File(presetsDir(context), "$name.json")
        return if (file.exists()) file.readText() else null
    }

    private fun restorePluginFromJson(pathId: Long, pluginIndex: Int, pluginObj: JSONObject): Boolean {
        // Control ports
        val controlPortsArr = pluginObj.optJSONArray("controlPorts")
        val portCount = controlPortsArr?.length() ?: 0
        val portValues = FloatArray(portCount)
        val portIndices = IntArray(portCount)
        for (i in 0 until portCount) {
            val cp = controlPortsArr!!.getJSONObject(i)
            portIndices[i] = cp.getInt("index")
            portValues[i] = cp.getDouble("value").toFloat()
        }

        // State properties
        val propsArr = pluginObj.optJSONArray("stateProperties")
        val propCount = propsArr?.length() ?: 0
        val propKeys = Array(propCount) { "" }
        val propTypes = Array(propCount) { "" }
        val propValues = Array(propCount) { ByteArray(0) }
        val propFlags = IntArray(propCount)
        for (i in 0 until propCount) {
            val prop = propsArr!!.getJSONObject(i)
            propKeys[i] = prop.getString("key")
            propTypes[i] = prop.getString("type")
            propFlags[i] = prop.optInt("flags", 0)

            val encoding = prop.optString("encoding", "")
            val valueStr = prop.optString("value", "")
            propValues[i] = if (encoding == "base64") {
                Base64.decode(valueStr, Base64.DEFAULT)
            } else {
                // String value — add null terminator for LV2 compatibility
                (valueStr + "\u0000").toByteArray(Charsets.UTF_8)
            }
        }

        return engine.restorePluginState(
            pathId, pluginIndex, portValues, portIndices,
            propKeys, propTypes, propValues, propFlags
        )
    }
}
