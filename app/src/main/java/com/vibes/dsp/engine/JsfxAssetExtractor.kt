/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
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
import android.util.AtomicFile
import android.util.Log
import java.io.File
import java.io.FileInputStream
import java.io.InputStream

object JsfxAssetExtractor {
    private const val TAG = "JsfxAssetExtractor"
    private const val ASSETS_JSFX_ROOT = "jsfx"
    private const val FILES_JSFX_ROOT = "jsfx"
    private const val EFFECTS_DIR = "Effects"
    private const val DATA_DIR = "Data"
    private const val STAMP_FILE = ".jsfx_assets_stamp"
    private const val BUFFER_SIZE = 32 * 1024

    fun ensureJsfxAssetsExtracted(context: Context) {
        val targetRoot = File(context.filesDir, FILES_JSFX_ROOT)
        ensureDirectory(targetRoot)

        ensureDirectory(File(targetRoot, EFFECTS_DIR))
        ensureDirectory(File(targetRoot, DATA_DIR))

        val manifest = collectBundledJsfxFiles(context)
        if (manifest.isEmpty()) {
            Log.d(TAG, "No bundled JSFX files found in assets/$ASSETS_JSFX_ROOT")
            return
        }

        val currentVersion = getVersionCode(context)
        val previousStamp = readStamp(targetRoot)
        val previousManifest = parseStampManifest(previousStamp)
        val nextStamp = buildStamp(currentVersion, manifest)
        if (previousStamp == nextStamp) {
            Log.d(TAG, "JSFX assets already synchronized for version $currentVersion")
            return
        }

        var updated = 0
        var failed = false

        for (relativePath in manifest) {
            val assetPath = "$ASSETS_JSFX_ROOT/$relativePath"
            val destination = File(targetRoot, relativePath)

            if (!assetBytesDiffer(context, assetPath, destination)) {
                continue
            }

            if (copyAsset(context, assetPath, destination)) {
                updated++
            } else {
                failed = true
            }
        }

        for (relativePath in previousManifest - manifest.toSet()) {
            if (removeManagedFile(targetRoot, relativePath)) {
                updated++
            } else {
                failed = true
            }
        }

        if (updated > 0) {
            Log.i(TAG, "Refreshed $updated bundled JSFX file(s) in ${targetRoot.absolutePath}")
        }

        if (!failed && !writeStamp(targetRoot, nextStamp)) {
            Log.w(TAG, "Failed to persist JSFX asset stamp")
        }
    }

    private fun collectBundledJsfxFiles(context: Context): List<String> {
        val files = ArrayList<String>()
        collectAssetFiles(context, ASSETS_JSFX_ROOT, "", files)
        return files.sorted()
    }

    private fun collectAssetFiles(context: Context, assetPath: String, relativePath: String, out: MutableList<String>) {
        val entries = runCatching { context.assets.list(assetPath) }.getOrNull() ?: return

        if (entries.isEmpty()) {
            if (!isAssetDirectory(context, assetPath) && relativePath.isNotBlank()) {
                out.add(relativePath)
            }
            return
        }

        for (entry in entries) {
            val childAssetPath = if (assetPath.isBlank()) entry else "$assetPath/$entry"
            val childRelativePath = if (relativePath.isBlank()) entry else "$relativePath/$entry"

            if (isAssetDirectory(context, childAssetPath)) {
                collectAssetFiles(context, childAssetPath, childRelativePath, out)
            } else {
                out.add(childRelativePath)
            }
        }
    }

    private fun isAssetDirectory(context: Context, assetPath: String): Boolean {
        val listed = runCatching { context.assets.list(assetPath) }.getOrNull() ?: return false
        if (listed.isNotEmpty()) {
            return true
        }

        return runCatching {
            context.assets.open(assetPath).use { Unit }
            false
        }.getOrElse { true }
    }

    private fun copyAsset(context: Context, assetPath: String, destination: File): Boolean {
        ensureDirectory(destination.parentFile)

        val atomicFile = AtomicFile(destination)
        return try {
            context.assets.open(assetPath).use { input ->
                val output = atomicFile.startWrite()
                try {
                    copyStream(input, output)
                    atomicFile.finishWrite(output)
                    true
                } catch (e: Exception) {
                    atomicFile.failWrite(output)
                    Log.w(TAG, "Failed to write bundled JSFX asset $assetPath: ${e.message}")
                    false
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "Failed to copy bundled JSFX asset $assetPath: ${e.message}")
            false
        }
    }

    private fun ensureDirectory(file: File?) {
        if (file == null) return
        if (file.exists()) return
        if (!file.mkdirs()) {
            Log.w(TAG, "Could not create JSFX directory: ${file.absolutePath}")
        }
    }

    private fun copyStream(input: InputStream, output: java.io.OutputStream) {
        val buffer = ByteArray(BUFFER_SIZE)
        while (true) {
            val read = input.read(buffer)
            if (read < 0) break
            output.write(buffer, 0, read)
        }
    }

    private fun assetBytesDiffer(context: Context, assetPath: String, destination: File): Boolean {
        if (!destination.isFile) return true
        return try {
            context.assets.open(assetPath).use { input ->
                FileInputStream(destination).use { fileInput ->
                    streamsDiffer(input, fileInput)
                }
            }
        } catch (e: Exception) {
            true
        }
    }

    private fun streamsDiffer(first: InputStream, second: InputStream): Boolean {
        val firstBuffer = ByteArray(BUFFER_SIZE)
        val secondBuffer = ByteArray(BUFFER_SIZE)
        while (true) {
            val firstRead = first.read(firstBuffer)
            val secondRead = second.read(secondBuffer)
            if (firstRead != secondRead) return true
            if (firstRead <= 0) return false
            for (index in 0 until firstRead) {
                if (firstBuffer[index] != secondBuffer[index]) return true
            }
        }
    }

    private fun getVersionCode(context: Context): Long {
        return runCatching { context.packageManager.getPackageInfo(context.packageName, 0).longVersionCode }
            .getOrElse {
                runCatching {
                    context.packageManager.getPackageInfo(context.packageName, 0).versionCode.toLong()
                }.getOrElse { 0L }
            }
    }

    private fun parseStampManifest(stamp: String?): Set<String> {
        if (stamp == null) return emptySet()
        val lines = stamp.lineSequence().toList()
        val count = lines.getOrNull(1)?.toIntOrNull() ?: return emptySet()
        if (count < 0 || lines.size < count + 2) return emptySet()
        return lines.drop(2).take(count).filterTo(linkedSetOf()) { relativePath ->
            relativePath.isNotBlank() &&
                !File(relativePath).isAbsolute &&
                relativePath.split('/').none { it == ".." || it.isBlank() }
        }
    }

    private fun removeManagedFile(targetRoot: File, relativePath: String): Boolean {
        val canonicalRoot = runCatching { targetRoot.canonicalFile }.getOrNull() ?: return false
        val target = runCatching { File(targetRoot, relativePath).canonicalFile }.getOrNull()
            ?: return false
        val prefix = canonicalRoot.path + File.separator
        if (!target.path.startsWith(prefix)) return false
        if (!target.exists()) return true
        if (!target.isFile || !target.delete()) return false
        var parent = target.parentFile
        while (parent != null && parent != canonicalRoot && parent.list()?.isEmpty() == true) {
            if (!parent.delete()) break
            parent = parent.parentFile
        }
        return true
    }

    private fun writeStamp(targetRoot: File, stamp: String): Boolean {
        val atomicFile = AtomicFile(File(targetRoot, STAMP_FILE))
        val output = runCatching { atomicFile.startWrite() }.getOrNull() ?: return false
        return try {
            output.write(stamp.toByteArray(Charsets.UTF_8))
            atomicFile.finishWrite(output)
            true
        } catch (t: Throwable) {
            atomicFile.failWrite(output)
            false
        }
    }

    private fun buildStamp(version: Long, manifest: List<String>): String {
        return "$version\n${manifest.size}\n${manifest.joinToString("\n") }"
    }

    private fun readStamp(targetRoot: File): String? {
        val stampFile = File(targetRoot, STAMP_FILE)
        if (!stampFile.exists()) return null
        return runCatching { stampFile.readText() }.getOrNull()?.trim()
    }
}
