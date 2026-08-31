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

package com.vibes.dsp.engine

import java.io.File
import java.io.FileInputStream
import java.util.Locale
import java.util.Properties

internal const val REPOSITORY_METADATA_FILE = ".repository.properties"

internal data class InstalledPluginFacets(
    val packageFormat: String,
    val packageId: String,
    val version: String,
    val versionDirectory: File,
    val name: String,
    val manufacturer: String,
    val tags: List<String>,
    val vstUuids: Set<String>,
)

internal fun readInstalledPluginFacets(filesDir: File): List<InstalledPluginFacets> = buildList {
    val repositoryRoot = File(filesDir, "plugin-repositories/installed")
    val jsfxRoot = File(filesDir, "jsfx/Effects/repository")
    readRepositoryInstalledPluginFacets(repositoryRoot, this)
    readJsfxInstalledPluginFacets(jsfxRoot, this)
}

private fun readRepositoryInstalledPluginFacets(formatRoot: File, out: MutableList<InstalledPluginFacets>) {
    formatRoot.listFiles().orEmpty()
        .filter { it.isDirectory && !it.name.startsWith(".") }
        .forEach { formatDir ->
            readRepositoryInstalledPluginPackageFacets(formatDir, out)
        }
}

private fun readRepositoryInstalledPluginPackageFacets(formatDir: File, out: MutableList<InstalledPluginFacets>) {
    val packageFormat = formatDir.name
    formatDir.listFiles().orEmpty()
        .filter { it.isDirectory && !it.name.startsWith(".") }
        .forEach { packageDir ->
            packageDir.listFiles().orEmpty()
                .filter { it.isDirectory && !it.name.startsWith(".") }
                .maxByOrNull { it.name }
                ?.let { versionDir ->
                    readInstalledPluginFacets(packageFormat, packageDir.name, versionDir)?.let(out::add)
                }
        }
}

private fun readJsfxInstalledPluginFacets(root: File, out: MutableList<InstalledPluginFacets>) {
    root.listFiles().orEmpty()
        .filter { it.isDirectory && !it.name.startsWith(".") }
        .forEach { packageDir ->
            packageDir.listFiles().orEmpty()
                .filter { it.isDirectory && !it.name.startsWith(".") }
                .maxByOrNull { it.name }
                ?.let { versionDir ->
                    readInstalledPluginFacets("jsfx", packageDir.name, versionDir)?.let(out::add)
                }
        }
}

private fun readInstalledPluginFacets(
    packageFormat: String,
    packageId: String,
    versionDir: File,
): InstalledPluginFacets? = runCatching {
    val metadataFile = File(versionDir, REPOSITORY_METADATA_FILE)
    val properties = Properties().apply {
        FileInputStream(metadataFile).use(::load)
    }

    val parsedFormat = properties.getProperty("format", "").trim()
    val parsedId = properties.getProperty("id", "").trim()
    val name = properties.getProperty("name", "").trim()
    val manufacturer = properties.getProperty("manufacturer", "").trim()

    require(parsedFormat.isNotBlank())
    require(parsedId.isNotBlank())
    require(name.isNotBlank())
    require(manufacturer.isNotBlank())
    require(parsedFormat == packageFormat)
    require(parsedId == packageId)

    InstalledPluginFacets(
        packageFormat = parsedFormat,
        packageId = parsedId,
        version = versionDir.name,
        versionDirectory = versionDir.canonicalFile,
        name = name,
        manufacturer = manufacturer,
        tags = readRepositoryTags(properties.getProperty("tags", "")),
        vstUuids = readRepositoryVstUuids(properties.getProperty("ownership.vstUuids", "")),
    )
}.getOrNull()

private fun readRepositoryTags(value: String): List<String> = splitCaseInsensitiveDeduplicate(value, '\u001f')

private fun readRepositoryVstUuids(value: String): Set<String> = splitCaseInsensitiveDeduplicate(value, ',').toSet()

private fun splitCaseInsensitiveDeduplicate(value: String, delimiter: Char): List<String> {
    val out = mutableListOf<String>()
    val seen = HashSet<String>()
    value.split(delimiter).forEach { token ->
        val normalized = token.trim()
        if (normalized.isBlank()) return@forEach
        val key = normalized.lowercase(Locale.ROOT)
        if (seen.add(key)) out.add(normalized)
    }
    return out
}
