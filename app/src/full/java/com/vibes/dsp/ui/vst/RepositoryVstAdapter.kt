package com.vibes.dsp.ui.vst

import android.content.Context
import com.vibes.dsp.engine.PluginRepositoryService
import com.vibes.dsp.engine.VerifiedRepositoryPayload
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
            vm.setRepositoryCompletionCallback { success ->
                CoroutineScope(Dispatchers.IO).launch {
                    runCatching {
                        repository.completeStagedWineInstall(
                            packageId, m.version, success,
                            if (success) null else "Installer cancelled or failed",
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
            val extracted = File(context.cacheDir, "repo-vst-${m.id}-${m.version}").apply { deleteRecursively(); mkdirs() }
            try {
                extractEntry(payload.file, m.entry, extracted)
                val file = File(extracted, m.entry).canonicalFile
                VstRegistry.importFile(context, file, m.name)
            } finally { extracted.deleteRecursively() }
        }
        when (result) {
            is ImportResult.Ok -> {
                runCatching { NativeEngine.getInstance().nativeRefreshPluginRegistry() }
                repository.completeStagedWineInstall(packageId, m.version, true)
                onResult(Result.Installed(result.displayName))
            }
            is ImportResult.Err -> {
                repository.completeStagedWineInstall(packageId, m.version, false, result.reason)
                onResult(Result.Error(result.reason))
            }
        }
    }

    private fun extractEntry(zip: File, entry: String, root: File) {
        require(entry.isNotBlank() && !entry.startsWith('/') && !entry.split('/').contains(".."))
        ZipInputStream(zip.inputStream().buffered()).use { input ->
            var found = false
            while (true) {
                val e = input.nextEntry ?: break
                val name = e.name.replace('\\', '/')
                val out = File(root, name).canonicalFile
                require(out.toPath().startsWith(root.canonicalFile.toPath()))
                if (!e.isDirectory) {
                    out.parentFile?.mkdirs()
                    if (name == entry) { out.outputStream().use { input.copyTo(it) }; found = true }
                }
                input.closeEntry()
            }
            require(found) { "Manifest entry not found: $entry" }
        }
    }
}
