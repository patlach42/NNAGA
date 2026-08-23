/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of NNAGA.
 *
 * NNAGA is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

package com.vibes.dsp.ui.dashboard

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewmodel.CreationExtras
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/** A repository source as presented by the dashboard. */
data class RepositorySourceItem(
    val id: String,
    val name: String,
    val url: String,
    val enabled: Boolean,
    val isCustom: Boolean,
    val errorMessage: String? = null,
)

enum class RepositoryPackageStatus {
    Available,
    Installed,
    Update,
    Error,
    Incompatible,
}

enum class RepositoryPackageOperation {
    Installing,
    Updating,
    Removing,
}

/**
 * A repository package. [id] must remain the engine identity in `format:id` form.
 */
data class RepositoryPackageItem(
    val id: String,
    val name: String,
    val format: String,
    val sourceName: String,
    val availableVersion: String?,
    val installedVersion: String?,
    val description: String?,
    val status: RepositoryPackageStatus,
    val operation: RepositoryPackageOperation? = null,
    val progress: Float? = null,
    val errorMessage: String? = null,
)

data class RepositorySnapshot(
    val sources: List<RepositorySourceItem> = emptyList(),
    val packages: List<RepositoryPackageItem> = emptyList(),
    val isLoading: Boolean = true,
    val isRefreshing: Boolean = false,
    val errorMessage: String? = null,
)

/**
 * Narrow boundary between the dashboard and repository core. Implementations own persistence,
 * downloads, verification, installation, and their authoritative progress state.
 */
interface RepositoryService {
    val snapshot: StateFlow<RepositorySnapshot>

    suspend fun refreshAll()
    suspend fun addSource(url: String)
    suspend fun setSourceEnabled(sourceId: String, enabled: Boolean)
    suspend fun refreshSource(sourceId: String)
    suspend fun removeSource(sourceId: String)
    suspend fun install(packageId: String)
    suspend fun update(packageId: String)
    suspend fun remove(packageId: String)
}

data class RepositoryActionState(
    val pendingActions: Set<String> = emptySet(),
    val errorMessage: String? = null,
) {
    fun isPending(key: String): Boolean = key in pendingActions
}

class RepositoryViewModel(
    private val repository: RepositoryService,
) : ViewModel() {
    val snapshot: StateFlow<RepositorySnapshot> = repository.snapshot

    private val _actionState = MutableStateFlow(RepositoryActionState())
    val actionState: StateFlow<RepositoryActionState> = _actionState.asStateFlow()

    init {
        refreshAll()
    }

    fun refreshAll() = runAction(REFRESH_ALL) { repository.refreshAll() }

    fun addSource(url: String) {
        val normalizedUrl = url.trim()
        if (normalizedUrl.isEmpty()) return
        runAction(ADD_SOURCE) { repository.addSource(normalizedUrl) }
    }

    fun setSourceEnabled(sourceId: String, enabled: Boolean) =
        runAction(sourceKey(sourceId)) { repository.setSourceEnabled(sourceId, enabled) }

    fun refreshSource(sourceId: String) =
        runAction(sourceKey(sourceId)) { repository.refreshSource(sourceId) }

    fun removeSource(sourceId: String) =
        runAction(sourceKey(sourceId)) { repository.removeSource(sourceId) }

    fun install(packageId: String) =
        runAction(packageKey(packageId)) { repository.install(packageId) }

    fun update(packageId: String) =
        runAction(packageKey(packageId)) { repository.update(packageId) }

    fun remove(packageId: String) =
        runAction(packageKey(packageId)) { repository.remove(packageId) }

    fun clearActionError() {
        _actionState.update { it.copy(errorMessage = null) }
    }

    private fun runAction(key: String, action: suspend () -> Unit) {
        if (_actionState.value.isPending(key)) return
        _actionState.update {
            it.copy(
                pendingActions = it.pendingActions + key,
                errorMessage = null,
            )
        }
        viewModelScope.launch {
            try {
                action()
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                _actionState.update {
                    it.copy(errorMessage = error.message ?: "Repository operation failed")
                }
            } finally {
                _actionState.update {
                    it.copy(pendingActions = it.pendingActions - key)
                }
            }
        }
    }

    companion object {
        const val REFRESH_ALL = "refresh:all"
        const val ADD_SOURCE = "source:add"

        fun sourceKey(sourceId: String): String = "source:$sourceId"
        fun packageKey(packageId: String): String = "package:$packageId"

        fun factory(repository: RepositoryService): ViewModelProvider.Factory =
            object : ViewModelProvider.Factory {
                @Suppress("UNCHECKED_CAST")
                override fun <T : ViewModel> create(
                    modelClass: Class<T>,
                    extras: CreationExtras,
                ): T {
                    require(modelClass.isAssignableFrom(RepositoryViewModel::class.java))
                    return RepositoryViewModel(repository) as T
                }
            }
    }
}
