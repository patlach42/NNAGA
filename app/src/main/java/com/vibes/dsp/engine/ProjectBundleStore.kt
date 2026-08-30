package com.vibes.dsp.engine

import android.content.Context
import java.io.BufferedInputStream
import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.util.UUID
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

/** Durable .nnaga bundle storage. Native bytes remain authoritative. */
class ProjectBundleStore(context: Context) {
    private val mediaDir = File(context.filesDir, "project-media").apply { mkdirs() }
    fun mediaDirectoryPath(): String = mediaDir.absolutePath
    private val cacheDir = context.cacheDir
    data class Bundle(val rackState: ByteArray, val assets: Map<String, File>)

    fun mediaFile(assetId: String): File? = if (SAFE_ID.matches(assetId)) File(mediaDir, assetId) else null

    fun importMedia(input: InputStream, extension: String): Pair<String, File> {
        require(extension == ".wav" || extension == ".mid") { "Unsupported media type" }
        val id = UUID.randomUUID().toString().replace("-", "") + extension
        val target = File(mediaDir, id)
        input.use { source -> target.outputStream().use { output -> copyBounded(source, output, MAX_ENTRY) } }
        return id to target
    }

    fun writeBundle(rackState: ByteArray, refs: Array<ProjectClipMediaRef>, destination: OutputStream) {
        require(rackState.size <= MAX_ENTRY) { "Rack state is too large" }
        val staging = File.createTempFile("project_", ".nnaga", cacheDir)
        try {
            ZipOutputStream(staging.outputStream().buffered()).use { zip ->
                zip.putNextEntry(ZipEntry("rack-state.bin")); zip.write(rackState); zip.closeEntry()
                refs.map { it.assetId }.distinct().forEach { id ->
                    require(SAFE_ID.matches(id)) { "Invalid asset ID" }
                    val file = mediaFile(id) ?: error("Invalid asset ID")
                    require(file.isFile) { "Missing project asset: $id" }
                    zip.putNextEntry(ZipEntry("media/$id")); file.inputStream().use { copyBounded(it, zip, MAX_ENTRY) }; zip.closeEntry()
                }
            }
            staging.inputStream().use { it.copyTo(destination) }
            destination.flush()
        } finally { staging.delete() }
    }

    fun readBundle(input: InputStream): Bundle {
        var rackState: ByteArray? = null
        val assets = linkedMapOf<String, File>()
        var total = 0L
        try {
            ZipInputStream(BufferedInputStream(input)).use { zip ->
                while (true) {
                    val entry = zip.nextEntry ?: break
                    if (entry.isDirectory || entry.size > MAX_ENTRY) throw IllegalArgumentException("Invalid project entry")
                    val name = entry.name
                    if (name == "rack-state.bin") {
                        require(rackState == null) { "Duplicate rack-state.bin" }
                        val bytes = zip.readBounded(MAX_ENTRY)
                        rackState = bytes
                        total += bytes.size
                    } else if (name.startsWith("media/") && SAFE_ID.matches(name.removePrefix("media/"))) {
                        val id = name.removePrefix("media/")
                        require(!assets.containsKey(id)) { "Duplicate project asset: $id" }
                        val file = File.createTempFile("project_asset_", ".tmp", cacheDir)
                        assets[id] = file
                        file.outputStream().use { copyBounded(zip, it, MAX_ENTRY) }
                        total += file.length()
                    } else throw IllegalArgumentException("Invalid project entry: $name")
                    zip.closeEntry()
                    if (total > MAX_TOTAL) throw IllegalArgumentException("Project is too large")
                }
            }
            return Bundle(rackState ?: throw IllegalArgumentException("Missing rack-state.bin"), assets)
        } catch (failure: Throwable) {
            assets.values.forEach { it.delete() }
            throw failure
        }
    }

    fun deleteStagedAssets(bundle: Bundle) {
        bundle.assets.values.forEach { it.delete() }
    }

    fun installAssets(bundle: Bundle): Map<String, File> {
        val backups = linkedMapOf<File, File>()
        val installed = linkedMapOf<String, File>()
        try {
            bundle.assets.forEach { (id, source) ->
                require(SAFE_ID.matches(id))
                val target = File(mediaDir, id)
                if (target.isFile) {
                    val backup = File.createTempFile("project_backup_", ".tmp", cacheDir)
                    if (!target.renameTo(backup)) {
                        backup.delete()
                        error("Unable to stage existing project asset: $id")
                    }
                    backups[target] = backup
                }
                require(source.renameTo(target)) { "Unable to install project asset: $id" }
                installed[id] = target
            }
            backups.values.forEach { it.delete() }
            return installed
        } catch (failure: Throwable) {
            installed.values.forEach { it.delete() }
            backups.forEach { (target, backup) ->
                if (backup.isFile) backup.renameTo(target)
            }
            deleteStagedAssets(bundle)
            throw failure
        }
    }

    /*
     * Asset IDs are part of the native project format. Installation is
     * transactional so a failed copy cannot leave a mixture of old/new media.
     */

    companion object {
        private const val MAX_ENTRY = 64L * 1024 * 1024
        private const val MAX_TOTAL = 256L * 1024 * 1024
        private val SAFE_ID = Regex("[A-Za-z0-9][A-Za-z0-9._-]{0,127}")
        private fun copyBounded(input: InputStream, output: OutputStream, max: Long) {
            val buffer = ByteArray(32 * 1024); var total = 0L
            while (true) { val n = input.read(buffer); if (n < 0) break; total += n; require(total <= max) { "Project entry is too large" }; output.write(buffer, 0, n) }
        }
        private fun InputStream.readBounded(max: Long): ByteArray {
            val out = java.io.ByteArrayOutputStream(); copyBounded(this, out, max); return out.toByteArray()
        }
    }
}
