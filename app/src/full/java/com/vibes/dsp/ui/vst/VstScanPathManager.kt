/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * Licensed under GPL v3 — see app/src/main/cpp/plugin/IPlugin.h for full notice.
 */

package com.vibes.dsp.ui.vst

import android.content.Context
import java.io.File
import java.util.LinkedHashSet

object VstScanPathManager {
    private const val PREFS_NAME = "vst_scan_paths"
    private const val KEY_SCAN_PATHS = "scanPaths"
    private const val PATH_SEPARATOR = "\n"

    /**
     * Defaults are intentionally narrow: most managed VST installs land in these trees,
     * and they keep scan work bounded to practical install scopes.
     */
    val DEFAULT_PATHS: List<String> = listOf(
        "Program Files",
        "Program Files (x86)",
    )

    fun readScanPaths(context: Context): List<String> {
        val raw = context
            .getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getString(KEY_SCAN_PATHS, null)

        return normalizeScanPaths(raw?.split(PATH_SEPARATOR) ?: emptyList())
    }

    /**
     * Store user-edited paths (deduped, normalized, fallback to defaults when empty)
     * and return the normalized list.
     */
    fun writeScanPaths(context: Context, paths: List<String>): List<String> {
        val normalized = normalizeScanPaths(paths)
        context
            .getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit()
            .putString(KEY_SCAN_PATHS, normalized.joinToString(PATH_SEPARATOR))
            .apply()
        return normalized
    }

    /**
     * Convert user-facing paths (e.g. `drive_c\\Program Files` / `C:\\Program Files`
     * / `/Program Files`) to filesystem roots under [prefixPath] to scan.
     */
    fun resolveScanRoots(prefixPath: String, scanPaths: List<String>): List<File> {
        val paths = normalizeScanPaths(scanPaths)
        val prefix = File(prefixPath)
        val driveC = File(prefix, "drive_c")
        val seen = HashSet<String>()
        val out = mutableListOf<File>()

        for (raw in paths) {
            val root = if (raw.isBlank()) driveC else File(driveC, raw)
            if (!root.exists() || !root.isDirectory) continue

            val key = root.absolutePath
            if (seen.add(key)) out.add(root)
        }

        return out
    }

    private fun normalizeScanPaths(paths: List<String>): List<String> {
        val out = LinkedHashSet<String>()
        for (path in paths) {
            val normalized = normalizeScanPath(path) ?: continue
            out.add(normalized)
        }
        if (out.isEmpty()) {
            out.addAll(DEFAULT_PATHS)
        }
        return out.toList()
    }

    /**
     * Normalize a single user-supplied path to an entry rooted at
     * `prefix/drive_c/<value>`. Returns empty string for `drive_c` root.
     */
    private fun normalizeScanPath(raw: String): String? {
        var p = raw.replace('\\', '/').trim()
        if (p.isBlank()) return null

        p = p.trim('/')
        if (p.isBlank() || p == "." || p.equals("drive_c", ignoreCase = true)) return ""

        // Strip Windows-style drive prefixes: C:\..., c:/..., etc.
        if (p.length >= 2 && p[1] == ':') {
            p = p.substring(2).trim('/')
        }

        if (p.startsWith("drive_c/", ignoreCase = true)) {
            p = p.substring("drive_c/".length)
        }

        val segments = p
            .trim('/')
            .split('/')
            .filter { it.isNotBlank() }
        if (segments.isEmpty()) return ""
        if (segments.any { it == "." || it == ".." }) return null

        return segments.joinToString("/")
    }

    fun displayPath(path: String): String = path.ifBlank { "drive_c" }
}
