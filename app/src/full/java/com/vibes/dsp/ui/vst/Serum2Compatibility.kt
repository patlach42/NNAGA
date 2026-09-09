/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * Licensed under GPL v3 — see app/src/main/cpp/plugin/IPlugin.h for full notice.
 */

package com.vibes.dsp.ui.vst

import android.content.Context
import android.util.AtomicFile
import android.util.Log
import org.json.JSONObject
import java.io.File

/** Applies the Wine-safe Serum 2 renderer settings without replacing other preferences. */
internal object Serum2Compatibility {
    private const val TAG = "Serum2Compatibility"
    private const val DIRECT_COMPOSITION_KEY = "Disable DirectComposition"
    private const val PARTIAL_REDRAW_KEY = "Disable Partial Redraw"

    fun isPluginPath(path: String): Boolean {
        val separator = maxOf(path.lastIndexOf('/'), path.lastIndexOf('\\'))
        val filename = path.substring(separator + 1)
        return filename.equals("Serum2.vst3", ignoreCase = true) ||
            filename.equals("Serum 2.vst3", ignoreCase = true)
    }

    /** Repair every registered Serum 2 prefix. Caller must run off the main thread. */
    fun applyToRegisteredPrefixes(context: Context, entries: List<VstRegistryEntry>) {
        entries.asSequence()
            .filter { isPluginPath(it.dllPath) }
            .map {
                it.prefixPath?.let(::File)
                    ?: File(context.filesDir, "wineprefix_v${it.uuid}")
            }
            .distinctBy { it.absolutePath }
            .forEach(::applyToPrefix)
    }

    /** Apply after an installer has drained, before its prefix is cloned or reused. */
    fun applyToPrefix(prefix: File) {
        val profiles = File(prefix, "drive_c/users").listFiles { file ->
            file.isDirectory && !file.name.equals("Public", ignoreCase = true)
        }
        if (profiles.isNullOrEmpty()) {
            Log.w(TAG, "No Wine user profile in ${prefix.name}")
            return
        }

        for (profile in profiles) {
            val prefs = File(
                profile,
                "AppData/Roaming/Xfer/Serum 2/Serum2Prefs.json",
            )
            val raw = if (prefs.isFile) {
                try {
                    prefs.readText(Charsets.UTF_8)
                } catch (t: Throwable) {
                    Log.e(TAG, "Cannot read preferences in ${prefix.name}/${profile.name}", t)
                    continue
                }
            } else {
                "{}"
            }
            val json = try {
                JSONObject(raw)
            } catch (t: Throwable) {
                Log.e(TAG, "Malformed preferences in ${prefix.name}/${profile.name}", t)
                continue
            }

            if (json.optBoolean(DIRECT_COMPOSITION_KEY, false) &&
                json.optBoolean(PARTIAL_REDRAW_KEY, false)
            ) {
                Log.i(TAG, "Already applied in ${prefix.name}/${profile.name}")
                continue
            }

            val parent = prefs.parentFile
            if (parent == null || (!parent.isDirectory && !parent.mkdirs())) {
                Log.e(TAG, "Cannot create preferences in ${prefix.name}/${profile.name}")
                continue
            }

            val updated = patchPreferences(raw, json.length() > 0)
            val atomic = AtomicFile(prefs)
            val output = try {
                atomic.startWrite()
            } catch (t: Throwable) {
                Log.e(TAG, "Cannot open ${prefix.name}/${profile.name}", t)
                continue
            }
            try {
                output.write(updated.toByteArray(Charsets.UTF_8))
                atomic.finishWrite(output)
                Log.i(TAG, "Applied in ${prefix.name}/${profile.name}")
            } catch (t: Throwable) {
                runCatching { atomic.failWrite(output) }
                Log.e(TAG, "Write failed in ${prefix.name}/${profile.name}", t)
            }
        }
    }

    /** Preserve Serum's own formatting and every unrelated preference byte. */
    internal fun patchPreferences(raw: String, hasExistingMembers: Boolean): String {
        var updated = raw
        val missing = mutableListOf<String>()
        for (key in listOf(DIRECT_COMPOSITION_KEY, PARTIAL_REDRAW_KEY)) {
            val valueRange = findTopLevelBoolean(updated, key)
            if (valueRange != null) {
                updated = updated.replaceRange(valueRange, "true")
            } else {
                missing += key
            }
        }
        if (missing.isEmpty()) return updated

        val close = updated.indexOfLast { !it.isWhitespace() }
        check(close >= 0 && updated[close] == '}')
        val beforeClose = updated.substring(0, close)
        val bodyEnd = beforeClose.indexOfLast { !it.isWhitespace() } + 1
        val suffix = updated.substring(bodyEnd)
        val lineEnding = if ("\r\n" in updated) "\r\n" else "\n"
        val indent = Regex("(?:\\r\\n|\\n)([ \\t]*)\"")
            .find(updated)
            ?.groupValues
            ?.get(1)
            .orEmpty()
        val members = missing.joinToString(",$lineEnding") { key ->
            "$indent\"$key\": true"
        }
        val separator = if (hasExistingMembers) "," else ""
        val closingLine = if (suffix.startsWith("\n") || suffix.startsWith("\r\n")) {
            ""
        } else {
            lineEnding
        }
        return beforeClose.substring(0, bodyEnd) +
            separator + lineEnding + members + closingLine + suffix
    }

    private fun findTopLevelBoolean(raw: String, key: String): IntRange? {
        val quotedKey = "\"$key\""
        var depth = 0
        var index = 0
        while (index < raw.length) {
            when (raw[index]) {
                '{', '[' -> depth++
                '}', ']' -> depth--
                '"' -> {
                    val start = index
                    index++
                    while (index < raw.length && raw[index] != '"') {
                        if (raw[index] == '\\') index++
                        index++
                    }
                    if (depth == 1 &&
                        index < raw.length &&
                        index - start + 1 == quotedKey.length &&
                        raw.regionMatches(start, quotedKey, 0, quotedKey.length)
                    ) {
                        var valueStart = index + 1
                        while (valueStart < raw.length && raw[valueStart].isWhitespace()) {
                            valueStart++
                        }
                        if (valueStart < raw.length && raw[valueStart] == ':') {
                            valueStart++
                            while (valueStart < raw.length && raw[valueStart].isWhitespace()) {
                                valueStart++
                            }
                            when {
                                raw.regionMatches(valueStart, "true", 0, 4) ->
                                    return valueStart..valueStart + 3
                                raw.regionMatches(valueStart, "false", 0, 5) ->
                                    return valueStart..valueStart + 4
                            }
                        }
                    }
                }
            }
            index++
        }
        return null
    }
}
