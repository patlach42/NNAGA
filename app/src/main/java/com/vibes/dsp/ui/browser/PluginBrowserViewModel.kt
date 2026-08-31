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

package com.vibes.dsp.ui.browser

import android.app.Application
import android.content.Context
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.vibes.dsp.engine.FavoritesManager
import com.vibes.dsp.engine.PluginInfo
import com.vibes.dsp.engine.RackManager
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject


/**
 * Mapping of plugin names to their categories based on GxPlugins.lv2 README.
 * Plugins are matched by checking if the plugin name contains the key (case-insensitive).
 */
/**
 * Plugin metadata loaded from assets.
 */
data class PluginMetadata(
    val descriptions: Map<String, String>,
    val thumbnails: Map<String, String>,
    val authors: Map<String, String>,
    val categories: Map<String, String>,
    /** displayName (lowercased) -> is64Bit, for imported Windows VSTs from the registry. */
    val archByName: Map<String, Boolean> = emptyMap()
)

object PluginCategoryMapping {

    const val AUTHOR_GXPLUGINS = "GxPlugins"
    const val AUTHOR_GUITARIX = "Guitarix"
    const val AUTHOR_NEURAL_AMP = "Neural Amp Modeler"
    const val AUTHOR_AIDA_DSP = "Aida DSP"
    const val AUTHOR_BRUMMER10 = "brummer10"
    const val AUTHOR_UNKNOWN = "Unknown"
    const val AUTHOR_WINDOWS_VST = "Windows VST"

    /** Maps LV2 plugin class names to display categories. */
    private val LV2_CLASS_TO_CATEGORY = mapOf(
        "DistortionPlugin" to "Distortion",
        "AmplifierPlugin" to "Amplifier",
        "SimulatorPlugin" to "Simulator",
        "DelayPlugin" to "Delay",
        "ModulatorPlugin" to "Modulator",
        "FilterPlugin" to "Filter",
        "ReverbPlugin" to "Reverb",
        "EQPlugin" to "EQ",
        "CompressorPlugin" to "Compressor",
        "PitchPlugin" to "Pitch",
        "ChorusPlugin" to "Modulator",
        "FlangerPlugin" to "Modulator",
        "PhaserPlugin" to "Modulator",
        "EnvelopePlugin" to "Dynamics",
        "GatePlugin" to "Dynamics",
        "ExpanderPlugin" to "Dynamics",
        "UtilityPlugin" to "Utility",
        "AnalyserPlugin" to "Utility"
    )

    /**
     * Determines the category for a plugin using metadata LV2 class.
     * Falls back to "Other" if no class is found.
     */
    fun getCategory(pluginName: String, metadataCategories: Map<String, String> = emptyMap()): String {
        val normalizedName = pluginName.lowercase()
        val lv2Class = metadataCategories.entries
            .find { it.key.lowercase() == normalizedName }
            ?.value
        if (lv2Class != null) {
            return LV2_CLASS_TO_CATEGORY[lv2Class] ?: "Other"
        }
        return "Other"
    }
    
    /**
     * Determines the author for a plugin.
     * Currently identifies GxPlugins based on name prefix.
     */
    fun getAuthor(pluginName: String, format: String = "", metadataAuthors: Map<String, String> = emptyMap()): String {
        // Windows VST plugins (VST2/VST3) group under their own "Windows VST" header,
        // regardless of vendor metadata, instead of scattering into Unknown / per-vendor groups.
        if (format == "VST2" || format == "VST3") return AUTHOR_WINDOWS_VST
        // Check metadata authors map first (case-insensitive)
        val normalizedName = pluginName.lowercase()
        metadataAuthors.entries.find { it.key.lowercase() == normalizedName }?.let {
            return it.value
        }
        return if (pluginName.startsWith("Gx", ignoreCase = true)) {
            AUTHOR_GXPLUGINS
        } else {
            AUTHOR_UNKNOWN
        }
    }
}

internal data class PluginBrowserEntry(
    val plugin: PluginInfo,
    val author: String,
    val tags: List<String>,
    val type: String
)

internal data class PluginBrowserFilters(
    val author: String? = null,
    val tags: Set<String> = emptySet(),
    val type: String? = null
)

internal fun resolvePluginRepositoryFacet(
    plugin: PluginInfo,
    facets: List<com.vibes.dsp.engine.InstalledPluginFacets>
): com.vibes.dsp.engine.InstalledPluginFacets? {
    val candidates = when (plugin.format) {
        "VST2", "VST3" -> facets.filter {
            plugin.id in it.vstUuids
        }
        "JSFX" -> Regex("""^repository/([^/]+)/([^/]+)/""").find(plugin.id)?.let { m ->
            facets.filter {
                it.packageFormat.equals("JSFX", true) &&
                    it.packageId == m.groupValues[1] && it.version == m.groupValues[2]
            }
        } ?: emptyList()
        "LV2", "NATIVE" -> {
            val origin = runCatching { java.io.File(plugin.originPath).canonicalFile }.getOrNull()
            if (origin == null) emptyList() else facets.filter {
                it.packageFormat.equals(plugin.format, true) &&
                    runCatching { origin.toPath().startsWith(it.versionDirectory.canonicalFile.toPath()) }.getOrDefault(false)
            }
        }
        else -> emptyList()
    }
    return candidates.singleOrNull()
}

internal fun makePluginBrowserEntry(
    plugin: PluginInfo,
    metadata: PluginMetadata,
    facets: List<com.vibes.dsp.engine.InstalledPluginFacets>
): PluginBrowserEntry {
    val facet = resolvePluginRepositoryFacet(plugin, facets)
    val staticAuthor = PluginCategoryMapping.getAuthor(plugin.name, plugin.format, metadata.authors)
    val author = facet?.manufacturer?.trim()?.takeIf { it.isNotBlank() && !it.equals("Unknown", true) } ?: staticAuthor
    val category = PluginCategoryMapping.getCategory(plugin.name, metadata.categories)
    val tags = (listOf(category).filterNot { it.equals("Other", true) } + (facet?.tags ?: emptyList()))
        .map(String::trim).filter(String::isNotBlank)
        .distinctBy { it.lowercase() }.sortedWith(String.CASE_INSENSITIVE_ORDER)
    return PluginBrowserEntry(plugin, author, tags, plugin.format)
}

internal fun computeVisibleEntries(
    entries: List<PluginBrowserEntry>,
    filters: PluginBrowserFilters,
    favorites: Set<String>
): List<PluginBrowserEntry> {
    val filtered = entries.filter { e ->
        (filters.author == null || e.author.equals(filters.author, true)) &&
            (filters.type == null || e.type.equals(filters.type, true)) &&
            filters.tags.all { wanted -> e.tags.any { it.equals(wanted, true) } }
    }
    return filtered.sortedWith(compareByDescending<PluginBrowserEntry> { it.plugin.fullId in favorites }
        .thenBy(String.CASE_INSENSITIVE_ORDER) { it.plugin.name }
        .thenBy { it.plugin.fullId })
}


class PluginBrowserViewModel(application: Application) : AndroidViewModel(application) {

    private val appContext = application.applicationContext
    private val _plugins = MutableStateFlow<List<PluginInfo>>(emptyList())
    val plugins: StateFlow<List<PluginInfo>> = _plugins.asStateFlow()
    private val _entries = MutableStateFlow<List<PluginBrowserEntry>>(emptyList())
    internal val entries: StateFlow<List<PluginBrowserEntry>> = _entries.asStateFlow()
    private val _visibleEntries = MutableStateFlow<List<PluginBrowserEntry>>(emptyList())
    internal val visibleEntries: StateFlow<List<PluginBrowserEntry>> = _visibleEntries.asStateFlow()
    private val _filters = MutableStateFlow(PluginBrowserFilters())
    internal val filters: StateFlow<PluginBrowserFilters> = _filters.asStateFlow()

    private val _authorOptions = MutableStateFlow<List<String>>(emptyList())
    val authorOptions: StateFlow<List<String>> = _authorOptions.asStateFlow()
    private val _tagOptions = MutableStateFlow<List<String>>(emptyList())
    val tagOptions: StateFlow<List<String>> = _tagOptions.asStateFlow()
    private val _typeOptions = MutableStateFlow<List<String>>(emptyList())
    val typeOptions: StateFlow<List<String>> = _typeOptions.asStateFlow()

    private val _isLoading = MutableStateFlow(false)
    val isLoading: StateFlow<Boolean> = _isLoading.asStateFlow()
    private val _errorMessage = MutableStateFlow<String?>(null)
    val errorMessage: StateFlow<String?> = _errorMessage.asStateFlow()
    private val _addFailureMessage = MutableStateFlow<String?>(null)
    val addFailureMessage: StateFlow<String?> = _addFailureMessage.asStateFlow()
    private val _blockingOperation = MutableStateFlow<String?>(null)
    val blockingOperation: StateFlow<String?> = _blockingOperation.asStateFlow()
    private val _favorites = MutableStateFlow<Set<String>>(emptySet())
    val favorites: StateFlow<Set<String>> = _favorites.asStateFlow()

    private var pluginMetadata: PluginMetadata? = null
    private var repositoryFacets: List<com.vibes.dsp.engine.InstalledPluginFacets> = emptyList()
    private var availablePlugins: Set<String> = emptySet()

    init {
        _favorites.value = FavoritesManager.getFavorites(appContext)
        viewModelScope.launch {
            withContext(Dispatchers.IO) { loadMetadata() }
            loadPluginsInternal()
        }
    }

    /** Each optional metadata source is isolated so a corrupt overlay cannot hide plugins. */
    private fun loadMetadata() {
        var static: PluginMetadata? = null
        runCatching {
            appContext.assets.open("plugin_metadata.json").use { input ->
                val json = JSONObject(input.bufferedReader().use { it.readText() })
                fun map(name: String) = buildMap {
                    json.optJSONObject(name)?.keys()?.forEach { put(it, json.optJSONObject(name)!!.optString(it, "")) }
                }
                val available = buildSet {
                    json.optJSONArray("availablePlugins")?.let { a ->
                        for (i in 0 until a.length()) add(a.optString(i, ""))
                    }
                }
                static = PluginMetadata(map("descriptions"), map("thumbnails"), map("authors"), map("categories"))
                availablePlugins = available
            }
        }.onFailure {
            android.util.Log.w("PluginBrowser", "Failed to load plugin metadata: ${it.message}")
            static = PluginMetadata(emptyMap(), emptyMap(), emptyMap(), emptyMap())
            availablePlugins = emptySet()
        }
        pluginMetadata = static

        repositoryFacets = runCatching {
            com.vibes.dsp.engine.readInstalledPluginFacets(appContext.filesDir)
        }.getOrElse {
            android.util.Log.w("PluginBrowser", "Failed to load repository facets: ${it.message}")
            emptyList()
        }

        val arch = runCatching {
            val result = mutableMapOf<String, Boolean>()
            val file = java.io.File(appContext.filesDir, "vst_plugins/registry.json")
            if (file.exists()) {
                JSONObject(file.readText()).optJSONArray("plugins")?.let { a ->
                    for (i in 0 until a.length()) {
                        val o = a.optJSONObject(i) ?: continue
                        o.optString("displayName").takeIf { it.isNotBlank() }?.let {
                            result[it.lowercase()] = o.optBoolean("is64Bit", true)
                        }
                    }
                }
            }
            result
        }.getOrElse {
            android.util.Log.w("PluginBrowser", "Failed to load VST registry overlay: ${it.message}")
            emptyMap()
        }
        pluginMetadata = (pluginMetadata ?: PluginMetadata(emptyMap(), emptyMap(), emptyMap(), emptyMap())).copy(archByName = arch)
    }

    private fun applyMetadata(plugin: PluginInfo): PluginInfo {
        val metadata = pluginMetadata ?: return plugin
        val key = plugin.name.lowercase()
        val description = metadata.descriptions.entries.firstOrNull { it.key.lowercase() == key }?.value ?: ""
        val thumbnail = metadata.thumbnails.entries.firstOrNull { it.key.lowercase() == key }?.value ?: ""
        val arch = when (plugin.format) {
            "VST2", "VST3" -> metadata.archByName[key]?.let { if (it) "x64" else "x86" } ?: ""
            "LV2", "NATIVE" -> "native"
            else -> ""
        }
        return plugin.copy(description = description, thumbnailPath = thumbnail, arch = arch)
    }


    private fun buildEntry(plugin: PluginInfo): PluginBrowserEntry {
        val metadata = pluginMetadata ?: PluginMetadata(emptyMap(), emptyMap(), emptyMap(), emptyMap())
        return makePluginBrowserEntry(plugin, metadata, repositoryFacets)
    }


    private suspend fun refreshInternal() = withContext(Dispatchers.IO) {
        loadMetadata()
        loadPluginsInternal()
    }

    private suspend fun loadPluginsInternal() {
        _isLoading.value = true
        _errorMessage.value = null
        try {
            val all = withContext(Dispatchers.IO) { RackManager.getAvailablePlugins() }
            val listed = availablePlugins.map { it.lowercase() }.toSet()
            val filtered = if (listed.isEmpty()) all else all.filter {
                it.format == "NATIVE" || listed.contains(it.name.lowercase())
            }
            val mapped = filtered.map(::applyMetadata)
            _plugins.value = mapped
            _entries.value = mapped.map(::buildEntry)
            updateFacetState()
            android.util.Log.i("PluginBrowser", "Showing ${mapped.size} out of ${all.size} discovered plugins")
        } catch (e: Exception) {
            _errorMessage.value = "Failed to load plugins: ${e.message}"
        } finally { _isLoading.value = false }
    }

    private fun updateFacetState() {
        val es = _entries.value
        fun distinct(values: List<String>) = values.map(String::trim).filter(String::isNotBlank)
            .distinctBy { it.lowercase() }.sortedWith(String.CASE_INSENSITIVE_ORDER)
        _authorOptions.value = distinct(es.map { it.author })
        _tagOptions.value = distinct(es.flatMap { it.tags })
        val preferred = listOf("NATIVE", "LV2", "JSFX", "VST2", "VST3")
        _typeOptions.value = preferred.filter { p -> es.any { it.type.equals(p, true) } } +
            distinct(es.map { it.type }).filterNot { candidate -> preferred.any { it.equals(candidate, true) } }
        val f = _filters.value
        val authors = _authorOptions.value
        val types = _typeOptions.value
        val tags = _tagOptions.value
        _filters.value = f.copy(
            author = f.author?.let { a -> authors.firstOrNull { it.equals(a, true) } },
            type = f.type?.let { t -> types.firstOrNull { it.equals(t, true) } },
            tags = f.tags.mapNotNull { t -> tags.firstOrNull { it.equals(t, true) } }.toSet()
        )
        recomputeVisible()
    }

    private fun recomputeVisible() {
        _visibleEntries.value = computeVisibleEntries(_entries.value, _filters.value, _favorites.value)
    }

    fun setAuthorFilter(author: String?) { _filters.value = _filters.value.copy(author = author); recomputeVisible() }
    fun toggleTagFilter(tag: String) {
        val existing = _filters.value.tags.firstOrNull { it.equals(tag, true) }
        _filters.value = _filters.value.copy(tags = if (existing == null) _filters.value.tags + tag else _filters.value.tags - existing)
        recomputeVisible()
    }
    fun setTypeFilter(type: String?) { _filters.value = _filters.value.copy(type = type); recomputeVisible() }
    fun clearFilters() { _filters.value = PluginBrowserFilters(); recomputeVisible() }

    private suspend fun <T> withBlockingOperation(label: String, block: suspend () -> T): T {
        _blockingOperation.value = label
        return try { block() } finally { _blockingOperation.value = null }
    }

    suspend fun addPluginToRack(pathId: Long, plugin: PluginInfo, position: Int = -1): Boolean {
        if (_blockingOperation.value != null) return false
        return withBlockingOperation("Adding plugin") {
            withContext(Dispatchers.IO) {
                try {
                    val index = RackManager.addPlugin(pathId, plugin.fullId, position)
                    if (index >= 0) true else { _addFailureMessage.value = "Could not add plugin. Plugin binaries (.so) are not included in this build—only metadata is available."; false }
                } catch (e: Exception) { _addFailureMessage.value = "Failed to add plugin: ${e.message}"; false }
            }
        }
    }

    suspend fun replacePluginInRack(pathId: Long, position: Int, plugin: PluginInfo): Boolean {
        if (_blockingOperation.value != null) return false
        return withBlockingOperation("Replacing plugin") {
            withContext(Dispatchers.IO) {
                try {
                    RackManager.removePlugin(pathId, position)
                    val index = RackManager.addPlugin(pathId, plugin.fullId, position)
                    if (index >= 0) true else { _addFailureMessage.value = "Could not add plugin. Plugin binaries (.so) are not included in this build—only metadata is available."; false }
                } catch (e: Exception) { _addFailureMessage.value = "Failed to replace plugin: ${e.message}"; false }
            }
        }
    }

    fun clearAddFailureMessage() { _addFailureMessage.value = null }
    fun toggleFavorite(pluginId: String) {
        FavoritesManager.toggleFavorite(appContext, pluginId)
        _favorites.value = FavoritesManager.getFavorites(appContext)
        recomputeVisible()
    }
    fun refresh() { viewModelScope.launch { refreshInternal() } }
}
