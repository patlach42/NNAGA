package com.vibes.dsp.engine

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import com.vibes.dsp.ui.rack.AudioImportDecoder
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.util.UUID

/** Directory-backed .nnaga projects. NNGS bytes remain authoritative. */
class ProjectBundleStore(private val context: Context) {
    private val resolver = context.contentResolver
    private val mediaDir = File(context.filesDir, "project-media").apply { mkdirs() }
    private val cacheDir = context.cacheDir
    private val metadata = context.getSharedPreferences("project-asset-sources", Context.MODE_PRIVATE)
    data class Bundle(val rackState: ByteArray, val assets: Map<String, File>)

    fun mediaDirectoryPath(): String = mediaDir.absolutePath
    fun mediaFile(assetId: String): File? = if (SAFE_ID.matches(assetId)) File(mediaDir, assetId) else null

    fun importMedia(input: InputStream, extension: String, sourceUri: Uri? = null): Pair<String, File> {
        require(extension == ".wav" || extension == ".mid") { "Unsupported media type" }
        val id = UUID.randomUUID().toString().replace("-", "")
        val target = File(mediaDir, id)
        input.use { source -> target.outputStream().use { output -> copyBounded(source, output, MAX_ENTRY) } }
        sourceUri?.let {
            if (hasReadablePersistedPermission(it)) {
                metadata.edit().putString(id, it.toString()).apply()
            }
        }
        return id to target
    }

    fun writeBundle(rackState: ByteArray, refs: Array<ProjectClipMediaRef>, parentTreeUri: Uri, projectName: String) {
        require(rackState.size <= MAX_ENTRY) { "Rack state is too large" }
        require(SAFE_NAME.matches(projectName)) { "Invalid project name" }
        val parent = requireDirectory(parentTreeUri)
        val normalizedProjectName = if (projectName.endsWith(".nnaga")) projectName else "$projectName.nnaga"
        val project = createChild(parent, normalizedProjectName, DocumentsContract.Document.MIME_TYPE_DIR)
        val clips = createChild(project, "clips", DocumentsContract.Document.MIME_TYPE_DIR)
        val manifest = JSONArray()
        var total = rackState.size.toLong()
        try {
            writeChild(project, "rack-state.bin", "application/octet-stream", rackState)
            refs.map { it.assetId }.distinct().forEach { id ->
                require(SAFE_ID.matches(id)) { "Invalid asset ID" }
                val source = mediaFile(id)
                val sourceUri = metadata.getString(id, null)
                val sourceUriUsable = sourceUri?.let { hasReadablePersistedPermission(Uri.parse(it)) } == true
                require(sourceUriUsable || source?.isFile == true) { "Missing project asset: $id" }
                val ref = refs.first { it.assetId == id }
                val clipFileName = makeClipFileName(id, ref.isMidi)
                val copied = ClipLauncherPreferencesBridge.copyClips(context)
                var childPath: String? = null
                if (copied || !sourceUriUsable) {
                    val sourceFile = requireNotNull(source) { "Missing project asset: $id" }
                    require(sourceFile.isFile) { "Missing project asset: $id" }
                    val child = createChild(clips, clipFileName, if (ref.isMidi) "audio/midi" else "audio/wav")
                    sourceFile.inputStream().use { input ->
                        resolver.openOutputStream(child)?.use { output ->
                            copyBounded(input, output, MAX_ENTRY)
                        } ?: error("Unable to write project clip")
                    }
                    childPath = "clips/$clipFileName"
                    total += sourceFile.length()
                    require(total <= MAX_TOTAL) { "Project is too large" }
                }
                manifest.put(JSONObject().apply {
                    put("assetId", id); put("trackId", ref.trackId); put("slot", ref.slot); put("isMidi", ref.isMidi)
                    if (sourceUriUsable) put("sourceUri", sourceUri)
                    childPath?.let { put("path", it) }
                })
            }
            writeChild(project, "nnaga.json", "application/json", JSONObject().apply { put("version", 1); put("assets", manifest) }.toString().toByteArray(Charsets.UTF_8))
        } catch (failure: Throwable) {
            // A partial child is never a valid project because nnaga.json is the commit marker.
            deleteTree(project)
            throw failure
        }
    }

    fun readBundle(projectTreeUri: Uri): Bundle {
        val project = requireDirectory(projectTreeUri)
        val name = queryName(project)
        require(name.endsWith(".nnaga")) { "Not an NNAGA project directory" }
        val manifestDoc = child(project, "nnaga.json") ?: error("Missing nnaga.json")
        val stateDoc = child(project, "rack-state.bin") ?: error("Missing rack-state.bin")
        val manifest = readBytes(manifestDoc, MAX_ENTRY).toString(Charsets.UTF_8).let { JSONObject(it) }

        require(manifest.keys().asSequence().toSet() == setOf("version", "assets") && manifest.getInt("version") == 1) { "Invalid project manifest" }
        val entries = manifest.getJSONArray("assets")
        val assets = linkedMapOf<String, File>(); var total = 0L
        try {
            for (i in 0 until entries.length()) {
                val item = entries.getJSONObject(i)
                require(item.keys().asSequence().toSet().all { it in setOf("assetId", "trackId", "slot", "isMidi", "sourceUri", "path") })
                val id = item.getString("assetId"); require(SAFE_ID.matches(id) && !assets.containsKey(id))
                val isMidi = item.getBoolean("isMidi")
                val path = item.optString("path", "")
                val source = item.optString("sourceUri", "").takeIf { it.isNotEmpty() }
                require(path.isNotEmpty() || source != null) { "Missing source for asset $id" }
                val staged = File.createTempFile("project_asset_", ".tmp", cacheDir)
                if (path.isNotEmpty()) {
                    val fileName = projectClipPathToFileName(path)
                    val doc = child(child(project, "clips") ?: error("Missing clips directory"), fileName) ?: error("Missing project clip: $fileName")
                    resolver.openInputStream(doc)?.use { input -> staged.outputStream().use { output -> copyBounded(input, output, MAX_ENTRY) } } ?: error("Unable to read project clip")
                } else {
                    val sourceUri = Uri.parse(requireNotNull(source))
                    resolver.openInputStream(sourceUri)?.use { input -> staged.outputStream().use { output -> copyBounded(input, output, MAX_ENTRY) } } ?: error("Unable to read source clip")
                    if (!isMidi) {
                        val decoded = File.createTempFile("project_decode_", ".wav", cacheDir)
                        try { AudioImportDecoder.copyOrDecode(context, sourceUri, decoded); staged.delete(); decoded.renameTo(staged) } finally { decoded.delete() }
                    }
                }
                assets[id] = staged; total += staged.length(); require(total <= MAX_TOTAL) { "Project is too large" }
            }
            return Bundle(readBytes(stateDoc, MAX_ENTRY), assets)
        } catch (failure: Throwable) { assets.values.forEach { it.delete() }; throw failure }
    }

    fun deleteStagedAssets(bundle: Bundle) = bundle.assets.values.forEach { it.delete() }
    fun installAssets(bundle: Bundle): Map<String, File> {
        val backups = linkedMapOf<File, File>(); val installed = linkedMapOf<String, File>()
        try {
            bundle.assets.forEach { (id, source) ->
                val target = mediaFile(id) ?: error("Invalid asset ID")
                if (target.isFile) { val backup = File.createTempFile("project_backup_", ".tmp", cacheDir); require(target.renameTo(backup)); backups[target] = backup }
                require(source.renameTo(target)); installed[id] = target
            }
            backups.values.forEach { it.delete() }; return installed
        } catch (failure: Throwable) { installed.values.forEach { it.delete() }; backups.forEach { (target, backup) -> if (backup.isFile) backup.renameTo(target) }; deleteStagedAssets(bundle); throw failure }
    }

    private fun requireDirectory(uri: Uri): Uri {
        require(DocumentsContract.isTreeUri(uri)) { "Invalid project directory" }
        val root = treeDocumentUri(uri)
        val type = resolver.getType(root)
        require(type == DocumentsContract.Document.MIME_TYPE_DIR) { "Invalid project directory" }
        return root
    }
    private fun queryName(uri: Uri): String = resolver.query(treeDocumentUri(uri), arrayOf(DocumentsContract.Document.COLUMN_DISPLAY_NAME), null, null, null)?.use { require(it.moveToFirst()); it.getString(0) } ?: error("Unable to inspect project")
    private fun child(parent: Uri, name: String): Uri? {
        val parentDocument = treeDocumentUri(parent)
        val childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(parentDocument, DocumentsContract.getDocumentId(parentDocument))
        return resolver.query(childrenUri, arrayOf(DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME), null, null, null)?.use { c ->
            val n = c.getColumnIndex(DocumentsContract.Document.COLUMN_DISPLAY_NAME)
            val d = c.getColumnIndex(DocumentsContract.Document.COLUMN_DOCUMENT_ID)
            while (c.moveToNext()) if (c.getString(n) == name) return@use DocumentsContract.buildDocumentUriUsingTree(parentDocument, c.getString(d))
            null
        }
    }
    private fun createChild(parent: Uri, name: String, mime: String): Uri {
        val parentDocument = treeDocumentUri(parent)
        require(child(parentDocument, name) == null) { "Project already exists: $name" }
        return DocumentsContract.createDocument(resolver, parentDocument, mime, name) ?: error("Unable to create $name")
    }
    private fun writeChild(parent: Uri, name: String, mime: String, bytes: ByteArray) { val doc = createChild(parent, name, mime); resolver.openOutputStream(doc)?.use { it.write(bytes) } ?: error("Unable to write $name") }
    private fun readBytes(uri: Uri, max: Long): ByteArray { val out = java.io.ByteArrayOutputStream(); resolver.openInputStream(uri)?.use { copyBounded(it, out, max) } ?: error("Unable to read project file"); return out.toByteArray() }
    private fun deleteTree(uri: Uri) { runCatching { DocumentsContract.deleteDocument(resolver, treeDocumentUri(uri)) } }
    private fun treeDocumentUri(uri: Uri): Uri {
        return if (DocumentsContract.isTreeUri(uri)) {
            val treeDocumentId = DocumentsContract.getTreeDocumentId(uri)
            require(treeDocumentId != null) { "Invalid project directory" }
            DocumentsContract.buildDocumentUriUsingTree(uri, treeDocumentId)
        } else uri
    }
    private fun copyBounded(input: InputStream, output: OutputStream, max: Long) { val buffer = ByteArray(32 * 1024); var total = 0L; while (true) { val n = input.read(buffer); if (n < 0) break; total += n; require(total <= max) { "Project entry is too large" }; output.write(buffer, 0, n) } }
    private fun hasReadablePersistedPermission(uri: Uri): Boolean = resolver.persistedUriPermissions.any { it.isReadPermission && it.uri == uri }
    private fun makeClipFileName(assetId: String, isMidi: Boolean): String {
        val extension = if (isMidi) ".mid" else ".wav"
        return if (assetId.endsWith(extension, ignoreCase = true)) assetId else "$assetId$extension"
    }
    private fun projectClipPathToFileName(path: String): String {
        require(path.startsWith("clips/")) { "Invalid project clip path: $path" }
        val fileName = path.removePrefix("clips/")
        require(fileName.isNotBlank() && !fileName.contains('/')) { "Invalid project clip path: $path" }
        return fileName
    }
    companion object { private const val MAX_ENTRY = 64L * 1024 * 1024; private const val MAX_TOTAL = 256L * 1024 * 1024; private val SAFE_ID = Regex("[A-Za-z0-9][A-Za-z0-9._-]{0,127}"); private val SAFE_NAME = Regex("[A-Za-z0-9][A-Za-z0-9 _.-]{0,63}") }
}

/** Avoid coupling storage to UI preference implementation details. */
private object ClipLauncherPreferencesBridge { fun copyClips(context: Context): Boolean = com.vibes.dsp.ui.live.ClipLauncherPreferences.getCopyClipsIntoProject(context) }
