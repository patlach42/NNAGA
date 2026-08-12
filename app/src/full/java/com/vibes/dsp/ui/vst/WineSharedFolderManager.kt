package com.vibes.dsp.ui.vst

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.DocumentsContract
import android.system.Os
import android.util.Log
import java.io.File
import java.io.IOException
import java.nio.file.Files

/** Persists and materializes a user-selected SAF tree for Wine. */
class WineSharedFolderManager(private val context: Context) {
    companion object {
        private const val TAG = "WineSharedFolder"
        private const val PREFS = "wine_shared_folder"
        private const val URI_KEY = "tree_uri"
        private const val MIRROR_NAME = "wine_shared"

        fun from(context: Context) = WineSharedFolderManager(context.applicationContext)
    }

    private val resolver get() = context.contentResolver
    private val prefs get() = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    val mirrorDirectory: File get() = File(context.filesDir, MIRROR_NAME)
    val selectedTreeUri: Uri? get() = prefs.getString(URI_KEY, null)?.let(Uri::parse)
    val hasSelection: Boolean get() = selectedTreeUri != null

    /** Call from ACTION_OPEN_DOCUMENT_TREE result; only directory trees are accepted. */
    fun setSelectedTreeUri(uri: Uri, takePersistablePermission: Boolean = true): Boolean {
        return try {
            if (takePersistablePermission) {
                val flags = Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                resolver.takePersistableUriPermission(uri, flags)
            }
            // Validate access before replacing the previous selection.
            val doc = DocumentsContract.buildDocumentUriUsingTree(uri, DocumentsContract.getTreeDocumentId(uri))
            resolver.query(doc, arrayOf(DocumentsContract.Document.COLUMN_MIME_TYPE), null, null, null)?.use { c ->
                if (!c.moveToFirst() || c.getString(0) != DocumentsContract.Document.MIME_TYPE_DIR) return false
            } ?: return false
            prefs.edit().putString(URI_KEY, uri.toString()).apply()
            true
        } catch (t: Throwable) {
            Log.w(TAG, "Unable to persist selected tree", t)
            false
        }
    }

    fun clearSelection() {
        selectedTreeUri?.let { uri -> runCatching { resolver.releasePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION) } }
        prefs.edit().remove(URI_KEY).apply()
        deleteTree(mirrorDirectory)
    }

    /** Materializes the selected tree into filesDir/wine_shared. Existing mirror is untouched on failure. */
    fun refresh(): Boolean {
        val tree = selectedTreeUri ?: return false
        val stage = File(context.filesDir, ".wine_shared_staging")
        return try {
            deleteTree(stage)
            if (!stage.mkdirs() && !stage.isDirectory) return false
            val rootId = DocumentsContract.getTreeDocumentId(tree)
            val root = DocumentsContract.buildDocumentUriUsingTree(tree, rootId)
            copyDocument(root, stage)
            val mirror = mirrorDirectory
            val old = File(context.filesDir, ".wine_shared_previous")
            deleteTree(old)
            if (mirror.exists() && !mirror.renameTo(old)) throw IOException("cannot stage old mirror")
            if (!stage.renameTo(mirror)) {
                old.renameTo(mirror)
                throw IOException("cannot install mirror")
            }
            deleteTree(old)
            true
        } catch (t: Throwable) {
            Log.w(TAG, "Unable to refresh shared folder", t)
            deleteTree(stage)
            false
        }
    }

    /** Refreshes the mirror and maps it as drive S: in each supplied Wine prefix. */
    fun refreshAndMount(prefixes: Iterable<File>): Boolean {
        if (selectedTreeUri == null) return false
        if (!refresh()) return false
        var ok = true
        prefixes.forEach { if (!mountIntoPrefix(it)) ok = false }
        return ok
    }

    fun mountIntoPrefix(prefix: File): Boolean {
        if (!mirrorDirectory.isDirectory) return false
        return try {
            val dos = File(prefix, "dosdevices")
            if (!dos.exists() && !dos.mkdirs()) return false
            val link = File(dos, "s:")
            Files.deleteIfExists(link.toPath())
            Os.symlink(mirrorDirectory.absolutePath, link.absolutePath)
            true
        } catch (t: Throwable) {
            Log.w(TAG, "Unable to map S: in ${prefix.absolutePath}", t)
            false
        }
    }

    private fun copyDocument(document: Uri, destination: File) {
        val mime: String? = resolver.query(document, arrayOf(DocumentsContract.Document.COLUMN_MIME_TYPE), null, null, null)?.use { c ->
            if (c.moveToFirst()) c.getString(0) else null
        } ?: throw IOException("SAF document unavailable")
        if (mime == DocumentsContract.Document.MIME_TYPE_DIR) {
            if (!destination.exists() && !destination.mkdirs()) throw IOException("cannot create directory")
            val id = DocumentsContract.getDocumentId(document)
            val children = DocumentsContract.buildChildDocumentsUriUsingTree(document, id)
            resolver.query(children, arrayOf(DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE), null, null, null)?.use { c ->
                val idCol = c.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DOCUMENT_ID)
                val nameCol = c.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DISPLAY_NAME)
                while (c.moveToNext()) {
                    val safe = safeName(c.getString(nameCol))
                    val child = DocumentsContract.buildDocumentUriUsingTree(document, c.getString(idCol))
                    copyDocument(child, File(destination, safe))
                }
            } ?: throw IOException("cannot enumerate SAF directory")
        } else {
            resolver.openInputStream(document)?.use { input -> destination.outputStream().use { input.copyTo(it) } }
                ?: throw IOException("cannot read SAF file")
        }
    }

    private fun safeName(name: String?): String {
        val clean = (name ?: "unnamed").replace('/', '_').replace('\\', '_').trim()
        return if (clean.isEmpty() || clean == "." || clean == "..") "unnamed" else clean.take(240)
    }

    private fun deleteTree(file: File) {
        if (!file.exists()) return
        if (Files.isSymbolicLink(file.toPath()) || file.isFile) { file.delete(); return }
        file.listFiles()?.forEach(::deleteTree)
        file.delete()
    }
}
