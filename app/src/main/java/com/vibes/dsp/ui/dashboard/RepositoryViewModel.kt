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
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
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
    val source: String? = null,
    val operation: RepositoryPackageOperation? = null,
    val progress: Float? = null,
    val errorMessage: String? = null,
    val manufacturer: String = "Unknown",
    val tags: List<String> = emptyList(),
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
    val packageOperations: Map<String, RepositoryPackageOperation> = emptyMap(),
    val packageErrors: Map<String, String> = emptyMap(),
    val errorMessage: String? = null,
) {
    fun isPending(key: String): Boolean = key in pendingActions
}

class RepositoryViewModel(
    private val repository: RepositoryService,
) : ViewModel() {
    private val _actionState = MutableStateFlow(RepositoryActionState())
    val actionState: StateFlow<RepositoryActionState> = _actionState.asStateFlow()

    val snapshot: StateFlow<RepositorySnapshot> =
        combine(repository.snapshot, actionState) { repositorySnapshot, actions ->
            repositorySnapshot.copy(
                packages = repositorySnapshot.packages.map { repositoryPackage ->
                    val operation = actions.packageOperations[repositoryPackage.id]
                    val error = actions.packageErrors[repositoryPackage.id]
                    repositoryPackage.copy(
                        status = if (error != null) {
                            RepositoryPackageStatus.Error
                        } else {
                            repositoryPackage.status
                        },
                        operation = operation ?: repositoryPackage.operation,
                        progress = if (operation != null) null else repositoryPackage.progress,
                        errorMessage = error ?: repositoryPackage.errorMessage,
                    )
                },
            )
        }.stateIn(
            scope = viewModelScope,
            started = SharingStarted.Eagerly,
            initialValue = repository.snapshot.value,
        )

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
        runPackageAction(packageId, RepositoryPackageOperation.Installing) {
            repository.install(packageId)
        }

    fun update(packageId: String) =
        runPackageAction(packageId, RepositoryPackageOperation.Updating) {
            repository.update(packageId)
        }

    fun remove(packageId: String) =
        runPackageAction(packageId, RepositoryPackageOperation.Removing) {
            repository.remove(packageId)
        }

    fun clearActionError() {
        _actionState.update { it.copy(errorMessage = null) }
    }

    private fun runPackageAction(
        packageId: String,
        operation: RepositoryPackageOperation,
        action: suspend () -> Unit,
    ) {
        val key = packageKey(packageId)
        if (_actionState.value.isPending(key)) return
        _actionState.update {
            it.copy(
                packageOperations = it.packageOperations + (packageId to operation),
                packageErrors = it.packageErrors - packageId,
            )
        }
        runAction(
            key = key,
            onError = { message ->
                _actionState.update {
                    it.copy(packageErrors = it.packageErrors + (packageId to message))
                }
            },
            onFinally = {
                _actionState.update {
                    it.copy(packageOperations = it.packageOperations - packageId)
                }
            },
        ) {
            action()
        }
    }

    private fun runAction(
        key: String,
        onError: (String) -> Unit = {},
        onFinally: () -> Unit = {},
        action: suspend () -> Unit,
    ) {
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
                val message = error.message ?: "Repository operation failed"
                onError(message)
                _actionState.update {
                    it.copy(errorMessage = message)
                }
            } finally {
                onFinally()
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
