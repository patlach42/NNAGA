package com.vibes.dsp.ui.vst

import android.content.Context
import com.vibes.dsp.engine.PluginRepositoryService
import com.vibes.dsp.engine.VerifiedRepositoryPayload
import com.vibes.dsp.engine.WineInstallOwnership
import com.vibes.dsp.engine.NativeEngine
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch
import java.io.File
import java.util.zip.ZipInputStream

/** Bridges verified repository payloads into the existing VST flows. */
object RepositoryVstAdapter {
    sealed class Result {
        data class Pending(val payload: VerifiedRepositoryPayload) : Result()
        data class Installed(val displayName: String) : Result()
        data class Error(val message: String) : Result()
    }
    fun removeOwnership(context: Context, ownership: WineInstallOwnership): Boolean {
        if (ownership.vstUuids.isEmpty() &&
            ownership.executableUuids.isEmpty() &&
            ownership.prefixPaths.isEmpty()
        ) return true
        return runCatching {
            val vst = VstRegistry.read(context)
            val executables = VstExecutableRegistry.read(context)
            val ownedVst = vst.filter { it.uuid in ownership.vstUuids }
            val ownedExecutables = executables.filter { it.uuid in ownership.executableUuids }
            ownedVst.forEach { VstRegistry.remove(context, it.uuid) }
            ownedExecutables.forEach { VstExecutableRegistry.remove(context, it.uuid, force = true) }
            ownership.prefixPaths.distinct().forEach { path ->
                val file = File(path).canonicalFile
                require(file.path.startsWith(context.filesDir.canonicalPath + File.separator))
                file.deleteRecursively()
                require(!file.exists())
            }
            true
        }.getOrDefault(false)
    }

    suspend fun stageAndHandle(
        context: Context,
        repository: PluginRepositoryService,
        packageId: String,
        installer: Any? = null,
        onResult: (Result) -> Unit,
    ) {
        val payload = try { repository.stageVerifiedPayload(packageId) }
            catch (e: Exception) { onResult(Result.Error(e.message ?: "Payload verification failed")); return }
        val m = payload.manifest
        if (m.format == "wine_installer") {
            val vm = installer as? VstInstallerViewModel
                ?: run {
                    repository.discardStagedPayload(payload)
                    onResult(Result.Error("Installer UI unavailable"))
                    return
                }
            val priorVst = VstRegistry.read(context).map { it.uuid }.toSet()
            val priorExecutables = VstExecutableRegistry.read(context).map { it.uuid }.toSet()
            vm.setRepositoryCompletionCallback(payload.file.absolutePath) { success ->
                CoroutineScope(Dispatchers.IO).launch {
                    runCatching {
                        val ownership = if (success) {
                            val vst = VstRegistry.read(context).filterNot { it.uuid in priorVst }
                            val exe = VstExecutableRegistry.read(context).filterNot { it.uuid in priorExecutables }
                            WineInstallOwnership(
                                vstUuids = vst.map { it.uuid },
                                executableUuids = exe.map { it.uuid },
                                prefixPaths = (vst.mapNotNull { it.prefixPath } +
                                    exe.map { it.prefixPath }).distinct(),
                            )
                        } else {
                            WineInstallOwnership()
                        }
                        repository.completeStagedWineInstall(
                            packageId, m.version, success,
                            if (success) null else "Installer cancelled or failed",
                            ownership,
                        )
                    }.onSuccess {
                        if (success) onResult(Result.Installed(m.name))
                        else onResult(Result.Error("Installer cancelled or failed"))
                    }.onFailure { onResult(Result.Error(it.message ?: "Install failed")) }
                }
            }
            vm.installFromExe(payload.file.absolutePath, m.name)
            onResult(Result.Pending(payload))
            return
        }
        val result = withContext(Dispatchers.IO) {
            val extracted = File(context.cacheDir, "repo-vst-${m.id}-${m.version}").apply {
                deleteRecursively()
                mkdirs()
            }
            try {
                extractEntry(payload.file, m.entry, extracted)
                val file = File(extracted, m.entry.replace('\\', '/')).canonicalFile
                VstRegistry.importFile(context, file, m.name)
            } finally {
                extracted.deleteRecursively()
            }
        }
        when (result) {
            is ImportResult.Ok -> {
                val refreshed = runCatching {
                    NativeEngine.getInstance().nativeRefreshPluginRegistry()
                }.getOrDefault(false)
                if (!refreshed) {
                    VstRegistry.remove(context, result.uuid)
                    repository.completeStagedWineInstall(
                        packageId, m.version, false, "Native plugin registry refresh failed",
                    )
                    onResult(Result.Error("Native plugin registry refresh failed"))
                } else {
                    runCatching {
                        repository.completeStagedWineInstall(
                            packageId,
                            m.version,
                            true,
                            ownership = WineInstallOwnership(vstUuids = listOf(result.uuid)),
                        )
                    }.onSuccess {
                        onResult(Result.Installed(result.displayName))
                    }.onFailure {
                        VstRegistry.remove(context, result.uuid)
                        onResult(Result.Error(it.message ?: "Install failed"))
                    }
                }
            }
            is ImportResult.Err -> {
                repository.completeStagedWineInstall(packageId, m.version, false, result.reason)
                onResult(Result.Error(result.reason))
            }
        }
    }

    private fun extractEntry(zip: File, entry: String, root: File) {
        val requested = normalizeEntry(entry)
        require(requested.isNotEmpty()) { "Manifest entry is empty" }
        ZipInputStream(zip.inputStream().buffered()).use { input ->
            val seen = HashSet<String>()
            var found = false
            var count = 0
            var total = 0L
            while (true) {
                val e = input.nextEntry ?: break
                require(++count <= MAX_ENTRIES) { "ZIP has too many entries" }
                val name = normalizeEntry(e.name)
                require(name.isNotEmpty()) { "ZIP entry is empty" }
                require(seen.add(name)) { "Duplicate ZIP entry: $name" }
                val out = File(root, name).canonicalFile
                require(out.toPath().startsWith(root.canonicalFile.toPath())) {
                    "ZIP entry escapes staging directory"
                }
                if (!e.isDirectory && name == requested) {
                    out.parentFile?.mkdirs()
                    out.outputStream().use { output ->
                        val buffer = ByteArray(BUFFER_SIZE)
                        while (true) {
                            val n = input.read(buffer)
                            if (n < 0) break
                            total += n
                            require(total <= MAX_EXTRACTED) { "ZIP exceeds extraction limit" }
                            output.write(buffer, 0, n)
                        }
                    }
                    found = true
                } else {
                    while (true) {
                        val n = input.read(DISCARD_BUFFER)
                        if (n < 0) break
                        total += n
                        require(total <= MAX_EXTRACTED) { "ZIP exceeds extraction limit" }
                    }
                }
                input.closeEntry()
            }
            require(found) { "Manifest entry not found: $requested" }
        }
    }

    private fun normalizeEntry(raw: String): String {
        val name = raw.replace('\\', '/').trimEnd('/')
        require(name.isNotBlank() && !name.startsWith('/')) { "Invalid ZIP entry" }
        val parts = name.split('/')
        require(parts.none { it.isEmpty() || it == "." || it == ".." }) { "Invalid ZIP entry" }
        return parts.joinToString("/")
    }

    private const val MAX_ENTRIES = 4096
    private const val MAX_EXTRACTED = 1024L * 1024 * 1024
    private const val BUFFER_SIZE = 8192
    private val DISCARD_BUFFER = ByteArray(BUFFER_SIZE)
}
