package com.vibes.dsp.ui.browser

import android.app.Application
import android.net.Uri
import android.provider.DocumentsContract
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.vibes.dsp.ui.live.ClipLauncherPreferences
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.util.Locale

internal data class ClipBrowserRow(
    val uri: Uri,
    val name: String,
    val mimeType: String,
    val isDirectory: Boolean,
    val depth: Int,
    val isExpanded: Boolean,
    val isLoading: Boolean,
)

internal data class ClipBrowserUiState(
    val rootUri: Uri? = null,
    val rootName: String = "",
    val rows: List<ClipBrowserRow> = emptyList(),
    val isLoading: Boolean = false,
    val errorMessage: String? = null,
)

internal class ClipBrowserViewModel(application: Application) : AndroidViewModel(application) {
    private data class Node(
        val uri: Uri,
        val name: String,
        val mimeType: String,
        val isDirectory: Boolean,
    )

    private val resolver = application.contentResolver
    private val _uiState = MutableStateFlow(ClipBrowserUiState())
    val uiState: StateFlow<ClipBrowserUiState> = _uiState.asStateFlow()

    private val operationMutex = Mutex()
    private var rootTreeUri: Uri? = null
    private var rootDocumentUri: Uri? = null
    private var rootName = ""
    private var childrenByParent = emptyMap<String, List<Node>>()
    private var expandedDirectories = emptySet<String>()
    private var loadingDirectories = emptySet<String>()
    private var treeGeneration = 0L

    fun ensureLoaded() {
        viewModelScope.launch { operationMutex.withLock { ensureLoadedLocked() } }
    }

    fun refresh() {
        viewModelScope.launch { operationMutex.withLock { reloadLocked() } }
    }

    fun toggleDirectory(uri: Uri) {
        viewModelScope.launch { operationMutex.withLock { toggleDirectoryLocked(uri) } }
    }

    private suspend fun ensureLoadedLocked() {
        val requestedRoot = ClipLauncherPreferences.getBrowserRootUri(getApplication())
        if (requestedRoot == rootTreeUri && rootDocumentUri != null) {
            publish()
        } else {
            reloadLocked(requestedRoot)
        }
    }

    private suspend fun reloadLocked(requestedRoot: Uri? = ClipLauncherPreferences.getBrowserRootUri(getApplication())) {
        val generation = ++treeGeneration
        rootTreeUri = requestedRoot
        rootDocumentUri = null
        rootName = ""
        childrenByParent = emptyMap()
        expandedDirectories = emptySet()
        loadingDirectories = emptySet()
        if (requestedRoot == null || !DocumentsContract.isTreeUri(requestedRoot)) {
            _uiState.value = ClipBrowserUiState()
            return
        }

        _uiState.value = ClipBrowserUiState(rootUri = requestedRoot, isLoading = true)
        val root = runCatching {
            withContext(Dispatchers.IO) {
                val documentId = DocumentsContract.getTreeDocumentId(requestedRoot)
                DocumentsContract.buildDocumentUriUsingTree(requestedRoot, documentId)
            }
        }.getOrElse {
            showRootError(generation)
            return
        }
        val result = runCatching {
            withContext(Dispatchers.IO) { queryChildren(requestedRoot, root) }
        }
        if (generation != treeGeneration) return
        result.onSuccess { (name, children) ->
            rootDocumentUri = root
            rootName = name
            childrenByParent = mapOf(root.toString() to children)
            publish()
        }.onFailure { showRootError(generation) }
    }

    private suspend fun toggleDirectoryLocked(uri: Uri) {
        val root = rootDocumentUri ?: return
        val key = uri.toString()
        if (key == root.toString()) return
        if (key in expandedDirectories) {
            expandedDirectories -= key
            publish()
            return
        }
        expandedDirectories += key
        if (key in childrenByParent) {
            publish()
            return
        }

        val tree = rootTreeUri ?: return
        val generation = treeGeneration
        loadingDirectories += key
        publish()
        val result = runCatching {
            withContext(Dispatchers.IO) { queryChildren(tree, uri) }
        }
        if (generation != treeGeneration) return
        loadingDirectories -= key
        result.onSuccess { (_, children) ->
            childrenByParent = childrenByParent + (key to children)
            publish()
        }.onFailure {
            expandedDirectories -= key
            publish("Unable to read this folder")
        }
    }

    private fun showRootError(generation: Long) {
        if (generation != treeGeneration) return
        rootDocumentUri = null
        childrenByParent = emptyMap()
        expandedDirectories = emptySet()
        loadingDirectories = emptySet()
        _uiState.value = ClipBrowserUiState(
            rootUri = rootTreeUri,
            errorMessage = "Folder access was lost. Choose it again in Settings > Clip Launcher.",
        )
    }

    private fun publish(errorMessage: String? = null) {
        val root = rootDocumentUri
        if (root == null) {
            _uiState.value = ClipBrowserUiState(rootUri = rootTreeUri, errorMessage = errorMessage)
            return
        }
        _uiState.value = ClipBrowserUiState(
            rootUri = rootTreeUri,
            rootName = rootName,
            rows = flatten(root, 0),
            isLoading = loadingDirectories.isNotEmpty(),
            errorMessage = errorMessage,
        )
    }

    private fun flatten(parent: Uri, depth: Int): List<ClipBrowserRow> {
        val children = childrenByParent[parent.toString()].orEmpty()
        return buildList {
            children.forEach { child ->
                val key = child.uri.toString()
                val expanded = child.isDirectory && key in expandedDirectories
                add(
                    ClipBrowserRow(
                        uri = child.uri,
                        name = child.name,
                        mimeType = child.mimeType,
                        isDirectory = child.isDirectory,
                        depth = depth,
                        isExpanded = expanded,
                        isLoading = key in loadingDirectories,
                    ),
                )
                if (expanded) addAll(flatten(child.uri, depth + 1))
            }
        }
    }

    private fun queryChildren(treeUri: Uri, parentUri: Uri): Pair<String, List<Node>> {
        val parentId = DocumentsContract.getDocumentId(parentUri)
        val parentName = resolver.query(
            parentUri,
            arrayOf(DocumentsContract.Document.COLUMN_DISPLAY_NAME),
            null,
            null,
            null,
        )?.use { cursor ->
            if (cursor.moveToFirst()) cursor.getString(0).orEmpty() else ""
        } ?: throw IllegalStateException("Unable to inspect folder")
        val childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, parentId)
        val children = resolver.query(
            childrenUri,
            arrayOf(
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE,
            ),
            null,
            null,
            null,
        )?.use { cursor ->
            val idIndex = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DOCUMENT_ID)
            val nameIndex = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DISPLAY_NAME)
            val mimeIndex = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_MIME_TYPE)
            buildList<Node> {
                while (cursor.moveToNext()) {
                    val documentId = cursor.getString(idIndex) ?: continue
                    val name = cursor.getString(nameIndex).orEmpty().ifBlank { "Unnamed" }
                    val mimeType = cursor.getString(mimeIndex).orEmpty()
                    val directory = mimeType == DocumentsContract.Document.MIME_TYPE_DIR
                    if (directory || isSupportedMedia(name, mimeType)) {
                        add(
                            Node(
                                uri = DocumentsContract.buildDocumentUriUsingTree(treeUri, documentId),
                                name = name,
                                mimeType = mimeType,
                                isDirectory = directory,
                            ),
                        )
                    }
                }
            }
        } ?: throw IllegalStateException("Unable to read folder")
        return parentName to children.sortedWith(
            compareBy<Node> { !it.isDirectory }.thenBy { it.name.lowercase(Locale.ROOT) },
        )
    }

    private fun isSupportedMedia(name: String, mimeType: String): Boolean {
        if (mimeType.startsWith("audio/")) return true
        if (mimeType in MIDI_MIME_TYPES) return true
        if (mimeType.isNotEmpty() && mimeType != "application/octet-stream") return false
        return name.substringAfterLast('.', "").lowercase(Locale.ROOT) in SUPPORTED_EXTENSIONS
    }

    private companion object {
        val MIDI_MIME_TYPES = setOf("audio/midi", "audio/x-midi", "application/x-midi")
        val SUPPORTED_EXTENSIONS = setOf(
            "wav", "mp3", "ogg", "oga", "m4a", "aac", "flac", "mid", "midi",
        )
    }
}
