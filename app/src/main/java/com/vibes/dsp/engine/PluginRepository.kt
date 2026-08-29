package com.vibes.dsp.engine

import android.content.Context
import android.util.Base64
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import org.tomlj.Toml
import java.io.BufferedInputStream
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.net.URI
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.security.MessageDigest
import java.util.Properties
import java.util.UUID
import java.util.zip.ZipInputStream
import com.vibes.dsp.ui.dashboard.*

internal fun resolveContainedRepositoryUrl(base: String, root: URI, relative: String): String {
    val u = URI(base).resolve(relative)
    val decoded = URI(u.scheme, u.userInfo, u.host, u.port, u.path, u.query, u.fragment)
    val r = URI(root.scheme, root.userInfo, root.host, root.port, root.path, null, null)
    require(decoded.scheme == r.scheme && decoded.host == r.host && decoded.port == r.port)
    val rootPath = r.normalize().path.trimEnd('/') + "/"
    val path = decoded.normalize().path
    require(path == rootPath.dropLast(1) || path.startsWith(rootPath))
    return decoded.normalize().toString()
}
internal fun resolveRepositoryPayloadUrl(base: String, root: URI, payload: String, allowExternalHttps: Boolean): String {
    val raw = URI(payload)
    if (allowExternalHttps && raw.isAbsolute) {
        require(raw.scheme.equals("https", ignoreCase = true) && !raw.host.isNullOrBlank()) { "Payload URL must be absolute HTTPS: $payload" }
        require(raw.userInfo == null && raw.query == null && raw.fragment == null && raw.path.isNotBlank()) { "Unsafe payload URL: $payload" }
        require(raw.normalize().path == raw.path && !raw.rawPath.orEmpty().contains("%2e", ignoreCase = true)) { "Unsafe payload URL: $payload" }
        return raw.normalize().toString()
    }
    return resolveContainedRepositoryUrl(base, root, payload)
}
internal fun validateDeclaredContentLength(length: Long, max: Long, url: String): Long {
    require(length == -1L || length in 1..max) {
        "Invalid declared content length $length (max $max) for $url"
    }
    return length
}


data class RepoManifestFile(
    val url: String,
    val sha256: String,
    val size: Long,
    val path: String,
)
private const val MAX_JSFX_FILE_SIZE = 512L * 1024 * 1024

internal fun validateFacetMetadata(manifest: PluginRepositoryService.RepoManifest) {
    fun safe(value: String, max: Int) = value.isNotBlank() && value.length <= max && value.none { it.isISOControl() }
    require(safe(manifest.manufacturer, 128))
    require(manifest.tags.size <= 32 && manifest.tags.all { safe(it, 64) })
}

internal data class RepositoryCatalogEntry(
    val manifest: String,
    val id: String,
    val name: String,
    val version: String,
    val format: String,
    val description: String,
    val manufacturer: String,
    val tags: List<String>,
    val sourceName: String,
    val source: String,
    val manifestUrl: String,
    val repositoryRoot: String,
)

internal fun validateRepositorySource(value: String): String {
    val uri = URI(value.trim())
    require(uri.scheme.equals("https", ignoreCase = true) && !uri.host.isNullOrBlank()) {
        "Source must be absolute HTTPS: $value"
    }
    require(uri.userInfo == null && uri.query == null && uri.fragment == null && uri.path.isNotBlank()) {
        "Unsafe source URL: $value"
    }
    require(!uri.path.endsWith(".git", ignoreCase = true))
    require(uri.normalize().path == uri.path && !uri.rawPath.orEmpty().contains("%2e", ignoreCase = true))
    return uri.normalize().toString()
}

internal fun parseRepositoryIndex(t: org.tomlj.TomlParseResult): List<RepositoryCatalogEntry> {
    require(!t.hasErrors()) { t.errors().joinToString("; ") }
    require(t.getLong("schema") == 2L)
    require(!t.getString("repository").isNullOrBlank())
    require(!t.getString("release").isNullOrBlank())
    val packages = t.getArray("packages") ?: throw IllegalArgumentException("Missing packages")
    require(packages.size() > 0)
    return packages.toList().map { value ->
        val table = value as? org.tomlj.TomlTable
            ?: throw IllegalArgumentException("Invalid package entry")
        fun required(key: String): String = table.getString(key)?.takeIf { it.isNotBlank() }
            ?: throw IllegalArgumentException("Missing package field: $key")
        val tags = (
            table.getArray("tags")
                ?: throw IllegalArgumentException("Missing package field: tags")
            ).toList().map {
            it as? String ?: throw IllegalArgumentException("Invalid tags element")
        }
        val source = validateRepositorySource(required("source"))
        val entry = RepositoryCatalogEntry(
            manifest = required("manifest"),
            id = required("id"),
            name = required("name"),
            version = required("version"),
            format = required("format"),
            description = required("description"),
            manufacturer = required("manufacturer"),
            tags = tags,
            sourceName = "",
            source = source,
            manifestUrl = "",
            repositoryRoot = "",
        )
        entry
    }.also { entries ->
        val identities = entries.map { "${it.format}:${it.id}" }
        require(identities.toSet().size == identities.size) { "Duplicate package identity" }
    }
}

internal fun parseRepositoryManifest(
    text: String,
    sourceName: String,
    manifestUrl: String,
    repositoryRoot: String,
): PluginRepositoryService.RepoManifest {
    val toml = Toml.parse(text)
    require(!toml.hasErrors()) { toml.errors().joinToString("; ") }
    val payload = toml.getTable("payload") ?: error("Missing payload")
    val install = toml.getTable("install") ?: error("Missing install")
    val manufacturer = when {
        !toml.contains("manufacturer") -> "Unknown"
        else -> toml.getString("manufacturer")
            ?.takeIf { it.isNotBlank() }
            ?: throw IllegalArgumentException("Invalid manufacturer")
    }
    val tags: List<String> = when {
        !toml.contains("tags") -> emptyList()
        else -> (toml.getArray("tags")
            ?: throw IllegalArgumentException("Invalid tags"))
            .toList()
            .map { value ->
                value as? String
                    ?: throw IllegalArgumentException("Invalid tags element")
            }
    }
    val files = payload.getArray("files")?.toList().orEmpty().map { value ->
        val file = value as? org.tomlj.TomlTable ?: throw IllegalArgumentException("Invalid files element")
        val path = file.getString("path").orEmpty().replace('\\', '/')
        val fileUrl = file.getString("url").orEmpty()
        val hash = file.getString("sha256")?.lowercase().orEmpty()
        val size = file.getLong("size") ?: 0L
        RepoManifestFile(fileUrl, hash, size, path)
    }
    val schema = toml.getLong("schema")?.toInt() ?: 0
    val declaredSource = toml.getString("source")?.takeIf { it.isNotBlank() }
        ?.let(::validateRepositorySource)
    require(declaredSource != null) { "Missing source" }
    return PluginRepositoryService.RepoManifest(
        schema = schema,
        id = toml.getString("id").orEmpty(),
        name = toml.getString("name").orEmpty(),
        version = toml.getString("version").orEmpty(),
        format = toml.getString("format").orEmpty(),
        description = toml.getString("description").orEmpty(),
        payloadUrl = payload.getString("url").orEmpty(),
        payloadSha256 = payload.getString("sha256")?.lowercase().orEmpty(),
        payloadSize = payload.getLong("size") ?: files.sumOf { it.size },
        entry = install.getString("entry").orEmpty(),
        kind = payload.getString("kind").orEmpty(),
        files = files,
        arch = toml.getArray("arch")?.toList()?.map(Any::toString).orEmpty(),
        sourceName = sourceName,
        source = declaredSource,
        manifestUrl = manifestUrl,
        repositoryRoot = repositoryRoot,
        manufacturer = manufacturer,
        tags = tags,
    ).also(::validateFacetMetadata)
}
internal fun validateRepositoryManifest(manifest: PluginRepositoryService.RepoManifest) {
    val packagePattern = Regex("[a-z0-9][a-z0-9._-]{0,127}")
    val versionPattern = Regex("[A-Za-z0-9][A-Za-z0-9._+-]{0,63}")
    val shaPattern = Regex("[0-9a-f]{64}")
    validateFacetMetadata(manifest)
    require(manifest.schema == 1 && manifest.id.isNotBlank() && manifest.name.isNotBlank())
    require(
        packagePattern.matches(manifest.id) &&
            packagePattern.matches(manifest.format) &&
            versionPattern.matches(manifest.version),
    )
    val source = requireNotNull(manifest.source) { "Missing source" }
    require(validateRepositorySource(source) == source) { "Non-canonical source" }
    val expectedKinds = when (manifest.format) {
        "lv2" -> setOf("archive")
        "wine_installer" -> setOf("installer")
        "wine_archive" -> setOf("archive")
        "wine_directory" -> setOf("directory")
        "jsfx" -> setOf("file", "files")
        else -> emptySet()
    }
    require(expectedKinds.isNotEmpty() && manifest.kind in expectedKinds)
    require(manifest.arch.contains("arm64-v8a"))
    if (manifest.kind != "files") {
        require(manifest.payloadSize in 1..512L * 1024 * 1024)
        require(shaPattern.matches(manifest.payloadSha256))
    } else {
        require(manifest.files.isNotEmpty())
        require(manifest.payloadSize in 1..512L * 1024 * 1024)
        val paths = mutableSetOf<String>()
        manifest.files.forEach { file ->
            val path = file.path.replace('\\', '/')
            val parts = path.split('/')
            require(path.isNotBlank() && !path.startsWith('/') && parts.all { it.isNotBlank() && it != "." && it != ".." })
            require(paths.add(path)) { "Duplicate JSFX file path: $path" }
            require(file.size in 1..MAX_JSFX_FILE_SIZE)
            require(shaPattern.matches(file.sha256))
            val u = URI(file.url)
            require(u.scheme.equals("https", ignoreCase = true) && !u.host.isNullOrBlank())
            require(u.userInfo == null && u.query == null && u.fragment == null && u.path.isNotBlank())
            require(u.normalize().path == u.path && !u.rawPath.orEmpty().contains("%2e", ignoreCase = true))
            if (manifest.format == "jsfx") {
                require(u.host.equals("raw.githubusercontent.com", ignoreCase = true)) { "Redirecting JSFX file URL: ${file.url}" }
            }
        }
        require(manifest.files.sumOf { it.size } == manifest.payloadSize)
    }
    if (manifest.format == "jsfx") {
        val entry = manifest.entry.replace('\\', '/')
        require(entry.isNotBlank() && !entry.startsWith('/') && !entry.split('/').contains(".."))
        when (manifest.kind) {
            "file" -> require(entry.endsWith(".jsfx"))
            "files" -> {
                require(manifest.files.any { it.path == entry })
                val name = entry.substringAfterLast('/')
                require(name.endsWith(".jsfx") || !name.contains('.'))
            }
        }
    }
}
internal data class RepositorySourceRecord(
    val id: String,
    val name: String,
    val url: String,
    val enabled: Boolean,
    val custom: Boolean,
    val lastError: String?,
)

internal fun migrateRepositorySources(
    stored: List<RepositorySourceRecord>,
    builtin: RepositorySourceRecord,
): List<RepositorySourceRecord> = stored.map { source ->
    if (source.id == builtin.id) builtin.copy(enabled = source.enabled) else source
}

/**
 * Preloads the mandatory DSP libraries referenced by LV2 manifests in [bundle].
 *
 * Android's linker requires libraries extracted under filesDir to be loaded with
 * their absolute path before Lilv attempts to dlopen them.  UI binaries use a
 * different predicate and are intentionally not loaded here.
 */
internal fun preloadLv2Binaries(bundle: File, loader: (String) -> Unit) {
    preloadLv2Binaries(bundle, loader, false)
}

internal fun preloadLv2Binaries(
    bundle: File,
    loader: (String) -> Unit,
    allowRewrittenFileUris: Boolean,
) {
    val root = bundle.canonicalFile
    require(root.isDirectory) { "LV2 bundle is not a directory: $root" }
    val binaries = linkedSetOf<String>()
    root.walkTopDown()
        .filter { it.isFile && it.extension.equals("ttl", ignoreCase = true) }
        .forEach ttlLoop@ { ttl ->
            val ttlFile = ttl.canonicalFile
            require(ttlFile.toPath().startsWith(root.toPath())) {
                "LV2 manifest escapes bundle: ${ttl.path}"
            }
            Regex("""\blv2:binary\s*<([^>]+)>""").findAll(ttlFile.readText(Charsets.UTF_8)).forEach matchLoop@ { match ->
                val reference = match.groupValues[1].trim()
                val uri = runCatching { URI(reference) }.getOrElse {
                    throw IllegalArgumentException("Invalid LV2 DSP binary reference: $reference", it)
                }
                require(reference.isNotEmpty() && uri.fragment == null && uri.query == null) {
                    "Unsafe LV2 DSP binary reference: $reference"
                }
                val binary = if (!uri.isAbsolute) {
                    require(reference.lowercase().endsWith(".so")) {
                        "LV2 DSP binary is not a shared library: $reference"
                    }
                    File(ttlFile.parentFile, reference).canonicalFile
                } else {
                    require(allowRewrittenFileUris && uri.scheme.equals("file", ignoreCase = true)) {
                        "Unsafe LV2 DSP binary reference: $reference"
                    }
                    if (!uri.rawAuthority.isNullOrEmpty()) return@matchLoop
                    val rewritten = runCatching { File(uri).canonicalFile }.getOrElse {
                        throw IllegalArgumentException("Invalid LV2 DSP binary reference: $reference", it)
                    }
                    if (!rewritten.toPath().startsWith(root.toPath())) return@matchLoop
                    require(reference.lowercase().endsWith(".so")) {
                        "LV2 DSP binary is not a shared library: $reference"
                    }
                    rewritten
                }
                require(binary.toPath().startsWith(root.toPath())) {
                    "LV2 DSP binary escapes bundle: $reference"
                }
                check(binary.isFile && binary.length() > 0) {
                    "Missing or empty LV2 DSP binary: $reference"
                }
                binaries += binary.path
            }
        }
        binaries.forEach { path ->
            try {
                loader(path)
            } catch (e: UnsatisfiedLinkError) {
                throw IllegalStateException("Failed to preload LV2 DSP binary: $path", e)
            }
        }
    }


/** Repository installer. All state mutations happen on IO and publish immutable snapshots. */
/** Verified repository payload handed to a flavor adapter for final installation. */
data class VerifiedRepositoryPayload(
    val packageId: String,
    val manifest: PluginRepositoryService.RepoManifest,
    val file: File,
    val token: String,
)
data class WineInstallOwnership(
    val vstUuids: List<String> = emptyList(),
    val executableUuids: List<String> = emptyList(),
    val prefixPaths: List<String> = emptyList(),
)

private data class StagedRepositoryPayload(
    val manifest: PluginRepositoryService.RepoManifest,
    val file: File,
)

class PluginRepositoryService(private val context: Context, private val nativeRefresh: (() -> Boolean)? = null, private val http: OkHttpClient = OkHttpClient(), private val removeWineOwnership: ((WineInstallOwnership) -> Boolean)? = null) : RepositoryService {
    private val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    private val root = File(context.filesDir, "plugin-repositories")
    private val installedRoot = File(root, "installed")
    private val jsfxInstalledRoot = File(context.filesDir, "jsfx/Effects/repository")
    private val state = MutableStateFlow(RepositorySnapshot())
    private val cache = mutableMapOf<String, RepoManifest>()
    private val catalog = mutableMapOf<String, RepositoryCatalogEntry>()
    private val stagedManifests = mutableMapOf<String, StagedRepositoryPayload>()
    private val mutex = Mutex()
    override val snapshot: StateFlow<RepositorySnapshot> = state
    override suspend fun refreshAll() = withContext(Dispatchers.IO) { mutex.withLock { refreshAllLocked() } }
    private fun refreshAllLocked() {
        state.value = state.value.copy(isLoading = true, isRefreshing = true, errorMessage = null)
        val sources = loadSources()
        val entries = mutableListOf<RepositoryCatalogEntry>()
        val updated = sources.toMutableList()
        var error: String? = null
        val ids = mutableSetOf<String>()
        sources.forEachIndexed { index, source ->
            if (!source.enabled) return@forEachIndexed
            try {
                val indexUrl = source.url
                val repositoryRoot = URI(indexUrl).let {
                    URI(it.scheme, it.userInfo, it.host, it.port, it.path, null, null)
                }.resolve("./")
                val parsed = parseRepositoryIndex(Toml.parse(fetchText(indexUrl))).map { entry ->
                    val manifestUrl = resolveContained(indexUrl, repositoryRoot, entry.manifest)
                    require(entry.format.matches(PACKAGE) && entry.id.matches(PACKAGE))
                    require(entry.version.matches(Regex("[A-Za-z0-9][A-Za-z0-9._+-]{0,63}")))
                    require(entry.manufacturer.length <= 128 && entry.manufacturer.none { it.isISOControl() })
                    require(entry.tags.size <= 32 && entry.tags.all { it.isNotBlank() && it.length <= 64 && it.none(Char::isISOControl) })
                    entry.copy(sourceName = source.name, manifestUrl = manifestUrl, repositoryRoot = repositoryRoot.toString())
                }
                val sourceIds = parsed.map { "${it.format}:${it.id}" }
                require(sourceIds.toSet().size == sourceIds.size) { "Duplicate package identity" }
                require(sourceIds.none { it in ids }) { "Duplicate package identity" }
                ids += sourceIds
                entries += parsed
                updated[index] = source.copy(lastError = null)
            } catch (e: Exception) {
                val msg = e.message ?: "refresh failed"
                error = error ?: msg
                updated[index] = source.copy(lastError = msg)
            }
        }
        saveSources(updated)
        publish(updated, entries, error)
    }
    override suspend fun addSource(url: String) = withContext(Dispatchers.IO) { mutex.withLock {
        val u = normalizeSource(url); val s = loadSources()
        require(s.none { it.url == u }) { "Source already exists" }
        saveSources(s + RepositorySourceRecord(sha256(u.toByteArray()).take(16), "Custom ${s.count { it.custom } + 1}", u, true, true, null))
        refreshAllLocked()
    } }
    override suspend fun setSourceEnabled(sourceId: String, enabled: Boolean) = withContext(Dispatchers.IO) { mutex.withLock {
        val s = loadSources(); require(s.any { it.id == sourceId })
        saveSources(s.map { if (it.id == sourceId) it.copy(enabled = enabled) else it }); refreshAllLocked()
    } }
    override suspend fun refreshSource(sourceId: String) = withContext(Dispatchers.IO) { mutex.withLock {
        require(loadSources().any { it.id == sourceId }); refreshAllLocked()
    } }
    override suspend fun removeSource(sourceId: String) = withContext(Dispatchers.IO) { mutex.withLock {
        val sources = loadSources()
        val source = sources.firstOrNull { it.id == sourceId } ?: error("Unknown source")
        require(source.custom)
        saveSources(sources.filterNot { it.id == sourceId })
        refreshAllLocked()
    } }
    override suspend fun install(packageId: String) { installOrUpdate(packageId, false) }
    override suspend fun update(packageId: String) { installOrUpdate(packageId, true) }
    /**
     * Download and verify a non-LV2 payload without claiming it installed.
     * The returned file is app-private and remains available to the flavor
     * adapter until it reports final VST registry success.
     */
    suspend fun stageVerifiedPayload(packageId: String): VerifiedRepositoryPayload =
        withContext(Dispatchers.IO) { mutex.withLock {
            val m = fullManifest(packageId)
            require(m.format != "lv2" && m.format != "jsfx") { "This payload uses the native repository installer" }
            setPackageState(packageId, RepositoryPackageOperation.Installing, 0f, null)
            val token = UUID.randomUUID().toString()
            val staged = File(root, ".pending-$token.payload").canonicalFile
            requireContained(staged, root)
            val tmp = File(root, ".download-$token.tmp")
            try {
                download(resolveRepositoryPayloadUrl(m.manifestUrl, URI(m.repositoryRoot), m.payloadUrl, false), tmp, m.payloadSize)
                require(sha256(tmp) == m.payloadSha256) { "Payload SHA-256 mismatch" }
                Files.move(tmp.toPath(), staged.toPath())
                stagedManifests[token] = StagedRepositoryPayload(m, staged)
                setPackageState(packageId, null, 1f, null)
                VerifiedRepositoryPayload(packageId, m, staged, token)
            } catch (e: Exception) {
                setPackageState(packageId, null, null, e.message ?: "Payload verification failed")
                throw e
            } finally {
                tmp.delete()
            }
        } }
    fun discardStagedPayload(payload: VerifiedRepositoryPayload) {
        val staged = stagedManifests.remove(payload.token)
        if (staged != null && staged.file.canonicalFile == payload.file.canonicalFile) staged.file.delete()
        else payload.file.delete()
        setPackageState(payload.packageId, null, null, null)
    }
    suspend fun completeStagedWineInstall(payload: VerifiedRepositoryPayload, success: Boolean, error: String? = null, ownership: WineInstallOwnership = WineInstallOwnership()) =
        withContext(Dispatchers.IO) { mutex.withLock {
            val stagedRecord = stagedManifests.remove(payload.token) ?: error("Staged payload is no longer available")
            require(stagedRecord.file.canonicalFile == payload.file.canonicalFile)
            val m = stagedRecord.manifest; require(m.version == payload.manifest.version); val packageId = payload.packageId; val staged = stagedRecord.file
            if (!success) { staged.delete(); setPackageState(packageId, null, null, error ?: "Wine install cancelled"); return@withLock }
            val dir = File(installedRoot, "${m.format}/${m.id}").canonicalFile; val target = File(dir, m.version).canonicalFile; requireContained(target, dir)
            val tmp = File(dir, ".${m.version}.${payload.token}.staging").canonicalFile; val backup = File(dir, ".${m.version}.${payload.token}.backup").canonicalFile; var committed = false
            try {
                tmp.mkdirs(); writeMetadata(tmp, m, ownership); dir.mkdirs(); if (target.exists()) Files.move(target.toPath(), backup.toPath(), StandardCopyOption.ATOMIC_MOVE); Files.move(tmp.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE); committed = true
                require(nativeRefresh?.invoke() != false) { "Native plugin registry refresh failed" }
                val prior = dir.listFiles().orEmpty().filter { it.isDirectory && !it.name.startsWith(".") && it != target }.mapNotNull(::readOwnership).fold(WineInstallOwnership()) { a, b -> mergeOwnership(a, b) }
                require(removeWineOwnership?.invoke(prior) != false) { "Prior Wine ownership removal failed" }; dir.listFiles().orEmpty().filter { it.isDirectory && !it.name.startsWith(".") && it != target }.forEach(File::deleteRecursively); backup.deleteRecursively(); staged.delete(); setPackageState(packageId, null, 1f, null); publishCurrent()
            } catch (e: Exception) {
                if (committed) target.deleteRecursively(); if (backup.exists() && !target.exists()) Files.move(backup.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE); if (ownership.vstUuids.isNotEmpty() || ownership.executableUuids.isNotEmpty() || ownership.prefixPaths.isNotEmpty()) removeWineOwnership?.invoke(ownership); nativeRefresh?.invoke(); setPackageState(packageId, null, null, e.message ?: "Install failed"); throw e
            } finally { tmp.deleteRecursively(); backup.deleteRecursively(); staged.delete() }
        } }
    override suspend fun remove(packageId: String) = withContext(Dispatchers.IO) { mutex.withLock {
        setPackageState(packageId, RepositoryPackageOperation.Removing, 0f, null); val p=packageId.split(':', limit=2); require(p.size==2 && PACKAGE.matches(p[0])); val removeRoot = if (p[0] == "jsfx") jsfxInstalledRoot else installedRoot; val relative = if (p[0] == "jsfx") p[1] else "${p[0]}/${p[1]}"; val dir=File(removeRoot,relative).canonicalFile; requireContained(dir,removeRoot); val backup=File(dir.parentFile,".${dir.name}.${UUID.randomUUID()}.backup"); val ownership = if (p[0].startsWith("wine_")) dir.listFiles().orEmpty().filter { it.isDirectory && !it.name.startsWith(".") }.mapNotNull(::readOwnership).fold(WineInstallOwnership()) { a, b -> mergeOwnership(a, b) } else WineInstallOwnership()
        try { if(dir.exists()) Files.move(dir.toPath(),backup.toPath(),StandardCopyOption.ATOMIC_MOVE); require(removeWineOwnership?.invoke(ownership) != false); require(nativeRefresh?.invoke()!=false); backup.deleteRecursively(); setPackageState(packageId,null,1f,null); publishCurrent() } catch(e:Exception) { if(backup.exists()&&!dir.exists()) Files.move(backup.toPath(),dir.toPath(),StandardCopyOption.ATOMIC_MOVE); nativeRefresh?.invoke(); setPackageState(packageId,null,null,e.message?:"Remove failed"); throw e }
    } }
    private suspend fun installOrUpdate(id: String, update: Boolean) = withContext(Dispatchers.IO) {
        mutex.withLock {
            val m = fullManifest(id); val installRoot = if (m.format == "jsfx") jsfxInstalledRoot else installedRoot; val relative = if (m.format == "jsfx") m.id else "${m.format}/${m.id}"; val dir = File(installRoot, relative).canonicalFile; requireContained(dir, installRoot); val target = File(dir, m.version).canonicalFile; requireContained(target, dir); require(!target.exists() || update); val stage = File(dir, ".${m.version}.${UUID.randomUUID()}.staging"); val backup = File(dir, ".${m.version}.${UUID.randomUUID()}.backup"); val tmp = File(root, ".download-${UUID.randomUUID()}.tmp"); var moved = false
            try {
                if (m.format == "jsfx" && m.kind == "files") {
                    m.files.forEach { declared -> val payload = File(stage, declared.path.replace('\\', '/')).canonicalFile; requireContained(payload, stage); payload.parentFile?.mkdirs(); val ft = File(root, ".download-${UUID.randomUUID()}.tmp"); try { download(resolveRepositoryPayloadUrl(m.manifestUrl, URI(m.repositoryRoot), declared.url, true), ft, declared.size); require(ft.length() == declared.size); require(sha256(ft) == declared.sha256); Files.move(ft.toPath(), payload.toPath(), StandardCopyOption.ATOMIC_MOVE) } finally { ft.delete() } }; validateEntry(stage, m.entry)
                } else {
                    download(
                        resolveRepositoryPayloadUrl(
                            m.manifestUrl,
                            URI(m.repositoryRoot),
                            m.payloadUrl,
                            false,
                        ),
                        tmp,
                        m.payloadSize,
                    )
                    require(tmp.length() == m.payloadSize) { "Payload size mismatch" }
                    require(sha256(tmp) == m.payloadSha256) { "Payload SHA-256 mismatch" }
                    if (m.format == "jsfx") {
                        require(m.kind == "file")
                        val payload = File(stage, m.entry).canonicalFile
                        requireContained(payload, stage)
                        payload.parentFile?.mkdirs()
                        Files.move(tmp.toPath(), payload.toPath(), StandardCopyOption.ATOMIC_MOVE)
                        require(payload.isFile)
                    } else {
                        extractSafe(tmp, stage)
                        validateEntry(stage, m.entry)
                    }
                }
                writeMetadata(stage, m); if (target.exists()) Files.move(target.toPath(), backup.toPath(), StandardCopyOption.ATOMIC_MOVE); Files.move(stage.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE); moved = true; if (m.format == "lv2") preloadLv2Binaries(target, System::load); require(nativeRefresh?.invoke() != false); if (backup.exists()) backup.deleteRecursively(); dir.listFiles().orEmpty().filter { it.isDirectory && !it.name.startsWith(".") && it != target }.forEach(File::deleteRecursively); setPackageState(id, null, 1f, null); publishCurrent()
            } catch (e: Exception) { if (moved) target.deleteRecursively(); if (backup.exists() && !target.exists()) Files.move(backup.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE); nativeRefresh?.invoke(); setPackageState(id, null, null, e.message ?: "Install failed"); throw e } finally { tmp.delete(); stage.deleteRecursively(); backup.deleteRecursively() }
        }
    }
    private fun fullManifest(packageId: String): RepoManifest {
        val expected = catalog[packageId] ?: error("Package is not available: $packageId")
        cache[packageId]?.takeIf { manifestMatchesSummary(it, expected) }?.let { return it }
        cache.remove(packageId)
        val manifest = parseManifest(
            fetchText(expected.manifestUrl),
            expected.sourceName,
            expected.manifestUrl,
            expected.repositoryRoot,
        )
        validate(manifest)
        require("${manifest.format}:${manifest.id}" == packageId) { "Manifest package identity mismatch" }
        require(manifestMatchesSummary(manifest, expected)) { "Manifest package metadata mismatch" }
        cache[packageId] = manifest
        return manifest
    }
    private fun manifestMatchesSummary(
        manifest: RepoManifest,
        entry: RepositoryCatalogEntry,
    ): Boolean =
        manifest.version == entry.version &&
            manifest.name == entry.name &&
            manifest.description == entry.description &&
            manifest.manufacturer == entry.manufacturer &&
            manifest.tags == entry.tags &&
            manifest.source == entry.source &&
            manifest.manifestUrl == entry.manifestUrl &&
            manifest.repositoryRoot == entry.repositoryRoot
    private fun publishCurrent(){ publish(loadSources(), catalog.values.toList(), null) }
    private fun publish(
        sources: List<RepositorySourceRecord>,
        entries: List<RepositoryCatalogEntry>,
        error: String?,
    ) {
        catalog.clear()
        entries.forEach { catalog["${it.format}:${it.id}"] = it }
        cache.entries.removeAll { (id, manifest) ->
            val entry = catalog[id]
            entry == null || !manifestMatchesSummary(manifest, entry)
        }
        val installed = inventory()
        val packages = entries.map { entry ->
            val id = "${entry.format}:${entry.id}"
            val installedVersion = installed[id]?.version
            val status = when {
                !com.vibes.dsp.BuildConfig.HAS_VST_HOST && entry.format.startsWith("wine_") ->
                    RepositoryPackageStatus.Incompatible
                installedVersion == null -> RepositoryPackageStatus.Available
                installedVersion == entry.version -> RepositoryPackageStatus.Installed
                else -> RepositoryPackageStatus.Update
            }
            RepositoryPackageItem(
                id = id,
                name = entry.name,
                format = entry.format,
                sourceName = entry.sourceName,
                source = entry.source,
                availableVersion = entry.version,
                installedVersion = installedVersion,
                description = entry.description,
                manufacturer = entry.manufacturer,
                tags = entry.tags,
                status = status,
            )
        } + installed.filterKeys { it !in catalog }.map { (id, installedPackage) ->
            val manifest = installedPackage.manifest
            RepositoryPackageItem(
                id = id,
                name = manifest.name,
                format = manifest.format,
                sourceName = manifest.sourceName,
                source = manifest.source,
                availableVersion = null,
                installedVersion = installedPackage.version,
                description = manifest.description,
                manufacturer = manifest.manufacturer,
                tags = manifest.tags,
                status = RepositoryPackageStatus.Installed,
            )
        }
        state.value = RepositorySnapshot(
            sources = sources.map {
                RepositorySourceItem(it.id, it.name, it.url, it.enabled, it.custom, it.lastError)
            },
            packages = packages,
            isLoading = false,
            isRefreshing = false,
            errorMessage = error,
        )
    }
    private fun inventory(): Map<String, Installed> {
        val out = mutableMapOf<String, Installed>()
        installedRoot.listFiles().orEmpty().forEach { format ->
            format.listFiles().orEmpty().forEach { id ->
                id.listFiles().orEmpty()
                    .filter { it.isDirectory && !it.name.startsWith(".") }
                    .maxByOrNull { it.name }
                    ?.let { versionDir ->
                        readMetadata(versionDir)?.let { manifest ->
                            out["${manifest.format}:${manifest.id}"] = Installed(versionDir.name, manifest)
                        }
                    }
            }
        }
        jsfxInstalledRoot.listFiles().orEmpty()
            .filter { it.isDirectory && !it.name.startsWith(".") }
            .forEach { id ->
                id.listFiles().orEmpty()
                    .filter { it.isDirectory && !it.name.startsWith(".") }
                    .maxByOrNull { it.name }
                    ?.let { versionDir ->
                        readMetadata(versionDir)?.let { manifest ->
                            out["jsfx:${id.name}"] = Installed(versionDir.name, manifest)
                        }
                    }
            }
        return out
    }
    private fun writeMetadata(dir:File,m:RepoManifest, ownership: WineInstallOwnership = WineInstallOwnership()){ Properties().apply{put("id",m.id);put("name",m.name);put("version",m.version);put("format",m.format);put("description",m.description);put("kind",m.kind);put("manufacturer",m.manufacturer);put("tags",m.tags.joinToString("\u001f"));m.source?.let { put("source", it) };put("ownership.vstUuids",ownership.vstUuids.joinToString(","));put("ownership.executableUuids",ownership.executableUuids.joinToString(","));put("ownership.prefixPaths",ownership.prefixPaths.joinToString(File.pathSeparator));store(FileOutputStream(File(dir,META)),null)} }
    private fun readOwnership(dir:File): WineInstallOwnership? = runCatching { Properties().apply { load(FileInputStream(File(dir,META))) }.let { p -> WineInstallOwnership(p.getProperty("ownership.vstUuids","").split(',').filter(String::isNotBlank),p.getProperty("ownership.executableUuids","").split(',').filter(String::isNotBlank),p.getProperty("ownership.prefixPaths","").split(File.pathSeparator).filter(String::isNotBlank)) } }.getOrNull()
    private fun mergeOwnership(a: WineInstallOwnership, b: WineInstallOwnership) = WineInstallOwnership(
        (a.vstUuids + b.vstUuids).distinct(),
        (a.executableUuids + b.executableUuids).distinct(),
        (a.prefixPaths + b.prefixPaths).distinct(),
    )
    private fun readMetadata(dir:File): RepoManifest? = runCatching {
        Properties().apply { load(FileInputStream(File(dir, META))) }.let { p ->
            RepoManifest(1, p.getProperty("id",""), p.getProperty("name",""), p.getProperty("version",""), p.getProperty("format",""), p.getProperty("description",""), "", "", 1, "", p.getProperty("kind","archive"), emptyList(), sourceName = "Installed", source = p.getProperty("source"), manufacturer = p.getProperty("manufacturer","Unknown"), tags = p.getProperty("tags","").split("\u001f").filter(String::isNotBlank))
        }
    }.getOrNull()
    private fun setPackageState(id:String,op:RepositoryPackageOperation?,progress:Float?,error:String?){state.value=state.value.copy(packages=state.value.packages.map{if(it.id==id)it.copy(operation=op,progress=progress,errorMessage=error,status=if(error!=null)RepositoryPackageStatus.Error else it.status)else it})}
    private fun validate(m:RepoManifest){ validateRepositoryManifest(m) }
    private fun parseManifest(text: String, source: String, url: String, repositoryRoot: String): RepoManifest =
        parseRepositoryManifest(text, source, url, repositoryRoot)
    private fun normalizeSource(u:String):String{val x=URI(u.trim());require(x.scheme in setOf("https","file"));require(x.fragment==null&&x.query==null);return x.toString()}
    private fun resolveContained(base:String,root:URI,relative:String):String =
        resolveContainedRepositoryUrl(base, root, relative)
    private fun fetchText(url:String):String = if(url.startsWith("file:")) {
        File(URI(url)).inputStream().use { input -> val out=java.io.ByteArrayOutputStream(); copyBounded(input,out,MAX_RESPONSE); out.toString(Charsets.UTF_8.name()) }
    } else http.newBuilder().followRedirects(false).followSslRedirects(false).build().newCall(Request.Builder().url(url).build()).execute().use{r->
        require(r.isSuccessful) { "HTTP ${r.code} $url" }; val body=r.body ?: error("Empty response body for $url"); validateDeclaredContentLength(body.contentLength(), MAX_RESPONSE, url)
        body.byteStream().use { input -> val out=java.io.ByteArrayOutputStream(); copyBounded(input,out,MAX_RESPONSE); out.toString(Charsets.UTF_8.name()) }
    }
    private fun download(url:String,out:File,max:Long){if(url.startsWith("file:")){File(URI(url)).inputStream().use{copyBounded(it,out,max)}}else http.newBuilder().followRedirects(false).followSslRedirects(false).build().newCall(Request.Builder().url(url).build()).execute().use{r->require(r.isSuccessful) { "HTTP ${r.code} $url" };val body=r.body ?: error("Empty response body for $url"); validateDeclaredContentLength(body.contentLength(), max, url); body.byteStream().use { copyBounded(it,out,max) }}}
    private fun copyBounded(input:java.io.InputStream,out:java.io.OutputStream,max:Long){val b=ByteArray(8192);var n=0L;while(true){val r=input.read(b);if(r<0)break;n+=r;require(n<=max);out.write(b,0,r)}}
    private fun copyBounded(input:java.io.InputStream,out:File,max:Long){out.outputStream().use { copyBounded(input,it,max) }}
    private fun extractSafe(zip:File,stage:File){stage.mkdirs();ZipInputStream(BufferedInputStream(FileInputStream(zip))).use{z->val seen=mutableSetOf<String>();var total=0L;var count=0;var e=z.nextEntry;while(e!=null){require(++count<=MAX_ENTRIES);val n=e.name.replace('\\','/');val c=File(stage,n).canonicalFile;require(n.isNotBlank()&&!n.startsWith('/')&&!n.split('/').contains(".."));require(seen.add(c.relativeTo(stage.canonicalFile).path));requireContained(c,stage);if(e.isDirectory)c.mkdirs()else{c.parentFile?.mkdirs();FileOutputStream(c).use{o->val b=ByteArray(8192);while(true){val r=z.read(b);if(r<0)break;total+=r;require(total<=MAX_EXTRACTED);o.write(b,0,r)}}};z.closeEntry();e=z.nextEntry}}}
    private fun validateEntry(stage:File,e:String){val f=File(stage,e).canonicalFile;requireContained(f,stage);require(f.isFile)}
    private fun requireContained(f:File,parent:File){require(f.toPath().startsWith(parent.canonicalFile.toPath()))}
    private fun saveSources(s:List<RepositorySourceRecord>){prefs.edit().putStringSet(SOURCES,s.map{encode(it)}.toSet()).commit()}
    private fun loadSources():List<RepositorySourceRecord>{
        val stored = prefs.getStringSet(SOURCES, null)?.mapNotNull(::decode)
        if (stored == null || stored.isEmpty()) return listOf(builtin())
        val migrated = migrateRepositorySources(stored, builtin())
        if (migrated != stored) saveSources(migrated)
        return migrated
    }
    private fun builtin(): RepositorySourceRecord =
        RepositorySourceRecord(
            id = "builtin",
            name = "NNAGA Plugin Repository",
            url = BUILTIN_INDEX_URL,
            enabled = true,
            custom = false,
            lastError = null,
        )
    data class RepoManifest(val schema:Int,val id:String,val name:String,val version:String,val format:String,val description:String,val payloadUrl:String,val payloadSha256:String,val payloadSize:Long,val entry:String,val kind:String,val arch:List<String>,val sourceName:String="",val source:String?=null,val manifestUrl:String="",val repositoryRoot:String="",val manufacturer:String="Unknown",val tags:List<String> = emptyList(),val files:List<RepoManifestFile> = emptyList())
    private fun encode(s:RepositorySourceRecord)=Base64.encodeToString(listOf(s.id,s.name,s.url,s.enabled,s.custom,s.lastError?:"").joinToString("\u0001").toByteArray(),Base64.NO_WRAP)
    private fun decode(v:String)=runCatching{String(Base64.decode(v,Base64.DEFAULT)).split('\u0001').let{RepositorySourceRecord(it[0],it[1],it[2],it[3].toBoolean(),it[4].toBoolean(),it.getOrNull(5)?.ifEmpty{null})}}.getOrNull()
    private data class Installed(val version:String,val manifest:RepoManifest)
    companion object{private const val BUILTIN_INDEX_URL = "https://raw.githubusercontent.com/patlach42/nnaga-plugin-repository/main/index.toml?v=2026-08-28";private const val PREFS="plugin_repository";private const val SOURCES="sources";private const val META=".repository.properties";private const val MAX_DOWNLOAD=512L*1024*1024;private const val MAX_RESPONSE=8L*1024*1024;private const val MAX_EXTRACTED=1024L*1024*1024;private const val MAX_ENTRIES=4096;private val PACKAGE=Regex("[a-z0-9][a-z0-9._-]{0,127}");private val VERSION=Regex("[A-Za-z0-9][A-Za-z0-9._+-]{0,63}");private val SHA=Regex("[0-9a-f]{64}")}
    private fun sha256(b:ByteArray)=digest(b.inputStream())
    private fun sha256(f:File)=digest(f.inputStream())
    private fun digest(i:java.io.InputStream):String { val md=MessageDigest.getInstance("SHA-256"); i.use { val buf=ByteArray(8192); while(true){val n=it.read(buf);if(n<0)break;md.update(buf,0,n)} }; return md.digest().joinToString(""){"%02x".format(it)} }
}
