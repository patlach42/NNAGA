package com.vibes.dsp.ui.vst

import android.content.Context
import com.vibes.dsp.engine.PluginRepositoryService
import com.vibes.dsp.engine.VerifiedRepositoryPayload

/** Play Store flavor has no Wine runtime and must never attempt repository VST installs. */
object RepositoryVstAdapter {
    sealed class Result {
        data class Pending(val payload: VerifiedRepositoryPayload) : Result()
        data class Installed(val displayName: String) : Result()
        data class Error(val message: String) : Result()
    }
    fun removeOwnership(
        context: Context,
        ownership: com.vibes.dsp.engine.WineInstallOwnership,
    ): Boolean = ownership.vstUuids.isEmpty() &&
        ownership.executableUuids.isEmpty() &&
        ownership.prefixPaths.isEmpty()

    suspend fun stageAndHandle(
        context: Context,
        repository: PluginRepositoryService,
        packageId: String,
        installer: Any? = null,
        onResult: (Result) -> Unit,
    ) {
        onResult(Result.Error("Incompatible: Wine VST hosting is available in the Full flavor only"))
    }
}
