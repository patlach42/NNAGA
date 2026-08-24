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
internal fun validateDeclaredContentLength(length: Long, max: Long, url: String): Long {
    require(length == -1L || length in 1..max) {
        "Invalid declared content length $length (max $max) for $url"
    }
    return length
}


internal fun validateFacetMetadata(manifest: PluginRepositoryService.RepoManifest) {
    fun safe(value: String, max: Int) = value.isNotBlank() && value.length <= max && value.none { it.isISOControl() }
    require(safe(manifest.manufacturer, 128))
    require(manifest.tags.size <= 32 && manifest.tags.all { safe(it, 64) })
}

internal fun parseRepositoryIndex(t: org.tomlj.TomlParseResult): List<String> {
    require(!t.hasErrors()) { t.errors().joinToString("; ") }
    require(t.getLong("schema") == 1L)
    val a = t.getArray("manifests") ?: error("Missing manifests")
    return a.toList().map { it.toString() }.also { require(it.isNotEmpty()) }
}

internal fun parseRepositoryManifest(
    text: String,
    source: String,
    url: String,
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
    return PluginRepositoryService.RepoManifest(
        schema = toml.getLong("schema")?.toInt() ?: 0,
        id = toml.getString("id").orEmpty(),
        name = toml.getString("name").orEmpty(),
        version = toml.getString("version").orEmpty(),
        format = toml.getString("format").orEmpty(),
        description = toml.getString("description").orEmpty(),
        payloadUrl = payload.getString("url").orEmpty(),
        payloadSha256 = payload.getString("sha256")?.lowercase().orEmpty(),
        payloadSize = payload.getLong("size") ?: 0L,
        entry = install.getString("entry").orEmpty(),
        kind = payload.getString("kind").orEmpty(),
        arch = toml.getArray("arch")?.toList()?.map(Any::toString).orEmpty(),
        manufacturer = manufacturer,
        tags = tags,
        sourceName = source,
        sourceUrl = url,
        repositoryRoot = repositoryRoot,
    ).also(::validateFacetMetadata)
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
    private val state = MutableStateFlow(RepositorySnapshot())
    private val cache = mutableMapOf<String, RepoManifest>()
    private val stagedManifests = mutableMapOf<String, StagedRepositoryPayload>()
    private val mutex = Mutex()
    override val snapshot: StateFlow<RepositorySnapshot> = state
    override suspend fun refreshAll() = withContext(Dispatchers.IO) { mutex.withLock { refreshAllLocked() } }
    private fun refreshAllLocked() {
        state.value = state.value.copy(isLoading = true, isRefreshing = true, errorMessage = null)
        val sources = loadSources(); val manifests = mutableListOf<RepoManifest>(); val updated = sources.toMutableList(); var error: String? = null; val ids = mutableSetOf<String>()
        sources.forEachIndexed { index, source ->
            if (!source.enabled) return@forEachIndexed
            try {
                val indexUrl = source.url
                val repositoryRoot = URI(indexUrl).let { URI(it.scheme, it.userInfo, it.host, it.port, it.path, null, null) }.resolve("./")
                val parsed = parseRepositoryIndex(Toml.parse(fetchText(indexUrl)))
                val sourceManifests = parsed.map { path ->
                    val manifestUrl = resolveContained(indexUrl, repositoryRoot, path)
                    parseManifest(fetchText(manifestUrl), source.name, manifestUrl, repositoryRoot.toString())
                }
                val sourceIds = sourceManifests.map { "${it.format}:${it.id}" }
                require(sourceIds.toSet().size == sourceIds.size) { "Duplicate package identity" }
                require(sourceIds.none { it in ids }) { "Duplicate package identity" }
                ids += sourceIds
                manifests += sourceManifests
                updated[index] = source.copy(lastError = null)
            } catch (e: Exception) {
                val msg = e.message ?: "refresh failed"; error = error ?: msg; updated[index] = source.copy(lastError = msg)
            }
        }
        saveSources(updated); publish(updated, manifests, error)
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
        val s = loadSources(); val x = s.firstOrNull { it.id == sourceId } ?: error("Unknown source")
        require(x.custom); saveSources(s.filterNot { it.id == sourceId }); refreshAllLocked()
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
            val m = cache[packageId] ?: error("Package is not available: $packageId")
            validate(m); require(m.format != "lv2") { "LV2 packages use the native repository installer" }
            setPackageState(packageId, RepositoryPackageOperation.Installing, 0f, null)
            val token = UUID.randomUUID().toString()
            val staged = File(root, ".pending-$token.payload").canonicalFile
            requireContained(staged, root)
            val tmp = File(root, ".download-$token.tmp")
            try {
                download(resolveContained(m.sourceUrl, URI(m.repositoryRoot), m.payloadUrl), tmp, m.payloadSize)
                require(sha256(tmp) == m.payloadSha256) { "Payload SHA-256 mismatch" }
                Files.move(tmp.toPath(), staged.toPath())
                stagedManifests[token] = StagedRepositoryPayload(m, staged)
                setPackageState(packageId, null, 1f, null)
                VerifiedRepositoryPayload(packageId, m, staged, token)
            } catch (e: Exception) {
                setPackageState(packageId, null, null, e.message ?: "Payload verification failed"); throw e
            } finally { tmp.delete() }
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
            val m = stagedRecord.manifest
            require(m.version == payload.manifest.version)
            val packageId = payload.packageId
            val staged = stagedRecord.file
            if (!success) { staged.delete(); setPackageState(packageId, null, null, error ?: "Wine install cancelled"); return@withLock }
            val dir = File(installedRoot, "${m.format}/${m.id}").canonicalFile
            val target = File(dir, m.version).canonicalFile; requireContained(target, dir)
            val tmp = File(dir, ".${m.version}.${payload.token}.staging").canonicalFile
            val backup = File(dir, ".${m.version}.${payload.token}.backup").canonicalFile
            var committed = false
            try {
                tmp.mkdirs(); writeMetadata(tmp, m, ownership); dir.mkdirs()
                if (target.exists()) Files.move(target.toPath(), backup.toPath(), StandardCopyOption.ATOMIC_MOVE)
                Files.move(tmp.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE); committed = true
                require(nativeRefresh?.invoke() != false) { "Native plugin registry refresh failed" }
                val prior = dir.listFiles().orEmpty().filter { it.isDirectory && !it.name.startsWith(".") && it != target }
                    .mapNotNull(::readOwnership).fold(WineInstallOwnership()) { a, b -> mergeOwnership(a, b) }
                require(removeWineOwnership?.invoke(prior) != false) { "Prior Wine ownership removal failed" }
                dir.listFiles().orEmpty().filter { it.isDirectory && !it.name.startsWith(".") && it != target }.forEach(File::deleteRecursively)
                backup.deleteRecursively(); staged.delete()
                setPackageState(packageId, null, 1f, null); publishCurrent()
            } catch (e: Exception) {
                if (committed) target.deleteRecursively()
                if (backup.exists() && !target.exists()) Files.move(backup.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE)
                if (ownership.vstUuids.isNotEmpty() || ownership.executableUuids.isNotEmpty() || ownership.prefixPaths.isNotEmpty()) removeWineOwnership?.invoke(ownership)
                nativeRefresh?.invoke(); setPackageState(packageId, null, null, e.message ?: "Install failed"); throw e
            } finally { tmp.deleteRecursively(); backup.deleteRecursively(); staged.delete() }
        } }


    override suspend fun remove(packageId: String) = withContext(Dispatchers.IO) { mutex.withLock {
        setPackageState(packageId, RepositoryPackageOperation.Removing, 0f, null); val p=packageId.split(':', limit=2); require(p.size==2); val dir=File(installedRoot,"${p[0]}/${p[1]}").canonicalFile; requireContained(dir,installedRoot)
        val backup=File(dir.parentFile,".${dir.name}.${UUID.randomUUID()}.backup")
        val ownership = dir.listFiles().orEmpty().filter { it.isDirectory && !it.name.startsWith(".") }.mapNotNull(::readOwnership)
            .fold(WineInstallOwnership()) { a, b -> mergeOwnership(a, b) }
        try { if(dir.exists()) Files.move(dir.toPath(),backup.toPath(),StandardCopyOption.ATOMIC_MOVE); require(removeWineOwnership?.invoke(ownership) != false) { "Wine ownership removal failed" }; require(nativeRefresh?.invoke()!=false) { "Native registry refresh failed" }; backup.deleteRecursively(); setPackageState(packageId,null,1f,null) ; publishCurrent() } catch(e:Exception) { if(backup.exists()&&!dir.exists()) Files.move(backup.toPath(),dir.toPath(),StandardCopyOption.ATOMIC_MOVE); nativeRefresh?.invoke(); setPackageState(packageId,null,null,e.message?:"Remove failed"); throw e }
    } }
    private suspend fun installOrUpdate(id:String, update:Boolean)=withContext(Dispatchers.IO) { mutex.withLock {
        val m=cache[id] ?: error("Package is not available: $id"); validate(m); val op=if(update) RepositoryPackageOperation.Updating else RepositoryPackageOperation.Installing; setPackageState(id,op,0f,null)
        val dir=File(installedRoot,"${m.format}/${m.id}").canonicalFile; requireContained(dir,installedRoot); val target=File(dir,m.version).canonicalFile; requireContained(target,dir); require(!target.exists()||update)
        val stage=File(dir,".${m.version}.${UUID.randomUUID()}.staging"); val backup=File(dir,".${m.version}.${UUID.randomUUID()}.backup"); val tmp=File(root,".download-${UUID.randomUUID()}.tmp"); var moved=false
        try { dir.mkdirs(); download(resolveContained(m.sourceUrl, URI(m.repositoryRoot),m.payloadUrl),tmp,m.payloadSize); require(sha256(tmp)==m.payloadSha256) { "Payload SHA-256 mismatch" }; extractSafe(tmp,stage); validateEntry(stage,m.entry); writeMetadata(stage,m); if(target.exists()) Files.move(target.toPath(),backup.toPath(),StandardCopyOption.ATOMIC_MOVE); Files.move(stage.toPath(),target.toPath(),StandardCopyOption.ATOMIC_MOVE); moved=true; require(nativeRefresh?.invoke()!=false) { "Native plugin registry refresh failed" }; if(backup.exists()) backup.deleteRecursively(); dir.listFiles().orEmpty().filter { it.isDirectory&&!it.name.startsWith(".")&&it!=target }.forEach(File::deleteRecursively); setPackageState(id,null,1f,null); publishCurrent()
        } catch(e:Exception){ if(moved) target.deleteRecursively(); if(backup.exists()&&!target.exists()) Files.move(backup.toPath(),target.toPath(),StandardCopyOption.ATOMIC_MOVE); nativeRefresh?.invoke(); setPackageState(id,null,null,e.message?:"Install failed"); throw e }
        finally { tmp.delete(); stage.deleteRecursively(); backup.deleteRecursively() }
    } }
    private fun publishCurrent(){ publish(loadSources(), cache.values.toList(), null) }
    private fun publish(
        sources: List<RepositorySourceRecord>,
        manifests: List<RepoManifest>,
        error: String?,
    ) {
        cache.clear()
        manifests.forEach { cache["${it.format}:${it.id}"] = it }
        val installed = inventory()
        val available = manifests.associateBy { "${it.format}:${it.id}" }
        val installedOnly = installed
            .filterKeys { it !in available }
            .mapValues { it.value.manifest }
        val all = available + installedOnly
        val packages = all.map { (id, manifest) ->
            val installedVersion = installed[id]?.version
            val status = when {
                !com.vibes.dsp.BuildConfig.HAS_VST_HOST && manifest.format.startsWith("wine_") ->
                    RepositoryPackageStatus.Incompatible
                installedVersion == null -> RepositoryPackageStatus.Available
                installedVersion == manifest.version -> RepositoryPackageStatus.Installed
                else -> RepositoryPackageStatus.Update
            }
            RepositoryPackageItem(
                id = id,
                name = manifest.name,
                format = manifest.format,
                sourceName = manifest.sourceName,
                availableVersion = manifest.version,
                installedVersion = installedVersion,
                description = manifest.description,
                manufacturer = manifest.manufacturer,
                tags = manifest.tags,
                status = status,
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
    private fun inventory():Map<String,Installed>{ val out=mutableMapOf<String,Installed>(); installedRoot.listFiles().orEmpty().forEach{f->f.listFiles().orEmpty().forEach{id->id.listFiles().orEmpty().filter{it.isDirectory&&!it.name.startsWith(".")}.maxByOrNull{it.name}?.let{v->readMetadata(v)?.let{m->out["${m.format}:${m.id}"]=Installed(v.name,m)}}}}; return out }
    private fun writeMetadata(dir:File,m:RepoManifest, ownership: WineInstallOwnership = WineInstallOwnership()){ Properties().apply{put("id",m.id);put("name",m.name);put("version",m.version);put("format",m.format);put("description",m.description);put("kind",m.kind);put("manufacturer",m.manufacturer);put("tags",m.tags.joinToString("\u001f"));put("ownership.vstUuids",ownership.vstUuids.joinToString(","));put("ownership.executableUuids",ownership.executableUuids.joinToString(","));put("ownership.prefixPaths",ownership.prefixPaths.joinToString(File.pathSeparator));store(FileOutputStream(File(dir,META)),null)} }
    private fun readOwnership(dir:File): WineInstallOwnership? = runCatching { Properties().apply { load(FileInputStream(File(dir,META))) }.let { p -> WineInstallOwnership(p.getProperty("ownership.vstUuids","").split(',').filter(String::isNotBlank),p.getProperty("ownership.executableUuids","").split(',').filter(String::isNotBlank),p.getProperty("ownership.prefixPaths","").split(File.pathSeparator).filter(String::isNotBlank)) } }.getOrNull()
    private fun mergeOwnership(a: WineInstallOwnership, b: WineInstallOwnership) = WineInstallOwnership(
        (a.vstUuids + b.vstUuids).distinct(),
        (a.executableUuids + b.executableUuids).distinct(),
        (a.prefixPaths + b.prefixPaths).distinct(),
    )
    private fun readMetadata(dir:File): RepoManifest? = runCatching {
        Properties().apply { load(FileInputStream(File(dir, META))) }.let { p ->
            RepoManifest(1, p.getProperty("id",""), p.getProperty("name",""), p.getProperty("version",""), p.getProperty("format",""), p.getProperty("description",""), "", "", 1, "", p.getProperty("kind","archive"), emptyList(), sourceName = "Installed", manufacturer = p.getProperty("manufacturer","Unknown"), tags = p.getProperty("tags","").split("\u001f").filter(String::isNotBlank))
        }
    }.getOrNull()
    private fun setPackageState(id:String,op:RepositoryPackageOperation?,progress:Float?,error:String?){state.value=state.value.copy(packages=state.value.packages.map{if(it.id==id)it.copy(operation=op,progress=progress,errorMessage=error,status=if(error!=null)RepositoryPackageStatus.Error else it.status)else it})}
    private fun validate(m:RepoManifest){validateFacetMetadata(m);require(m.schema==1&&m.id.isNotBlank()&&m.name.isNotBlank()&&m.version.isNotBlank());require(PACKAGE.matches(m.id)&&PACKAGE.matches(m.format)&&VERSION.matches(m.version));require(m.format in setOf("lv2","wine_installer","wine_archive","wine_directory"));require(m.kind==when(m.format){"lv2"->"archive";"wine_installer"->"installer";"wine_archive"->"archive";"wine_directory"->"directory";else->""}&&m.payloadSize in 1..MAX_DOWNLOAD);require(SHA.matches(m.payloadSha256));require(m.arch.contains("arm64-v8a"))}
    private fun parseManifest(text: String, source: String, url: String, repositoryRoot: String): RepoManifest =
        parseRepositoryManifest(text, source, url, repositoryRoot)
    private fun parseIndex(t:org.tomlj.TomlParseResult):List<String> = parseRepositoryIndex(t)
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
    private fun builtin()=RepositorySourceRecord("builtin","NNAGA Base",BUILTIN_INDEX_URL,true,false,null)
    private fun encode(s:RepositorySourceRecord)=Base64.encodeToString(listOf(s.id,s.name,s.url,s.enabled,s.custom,s.lastError?:"").joinToString("\u0001").toByteArray(),Base64.NO_WRAP)
    private fun decode(v:String)=runCatching{String(Base64.decode(v,Base64.DEFAULT)).split('\u0001').let{RepositorySourceRecord(it[0],it[1],it[2],it[3].toBoolean(),it[4].toBoolean(),it.getOrNull(5)?.ifEmpty{null})}}.getOrNull()
    data class RepoManifest(val schema:Int,val id:String,val name:String,val version:String,val format:String,val description:String,val payloadUrl:String,val payloadSha256:String,val payloadSize:Long,val entry:String,val kind:String,val arch:List<String>,val sourceName:String="",val sourceUrl:String="",val repositoryRoot:String="",val manufacturer:String="Unknown",val tags:List<String> = emptyList())
    private data class Installed(val version:String,val manifest:RepoManifest)
    companion object{private const val BUILTIN_INDEX_URL="https://raw.githubusercontent.com/patlach42/NNAGA/c2056dcf6c65ac8dd9236d67eee43a5ee1603ad9/plugin-repository/index.toml";private const val PREFS="plugin_repository";private const val SOURCES="sources";private const val META=".repository.properties";private const val MAX_DOWNLOAD=512L*1024*1024;private const val MAX_RESPONSE=8L*1024*1024;private const val MAX_EXTRACTED=1024L*1024*1024;private const val MAX_ENTRIES=4096;private val PACKAGE=Regex("[a-z0-9][a-z0-9._-]{0,127}");private val VERSION=Regex("[A-Za-z0-9][A-Za-z0-9._+-]{0,63}");private val SHA=Regex("[0-9a-f]{64}")}
    private fun sha256(b:ByteArray)=digest(b.inputStream())
    private fun sha256(f:File)=digest(f.inputStream())
    private fun digest(i:java.io.InputStream):String { val md=MessageDigest.getInstance("SHA-256"); i.use { val buf=ByteArray(8192); while(true){val n=it.read(buf);if(n<0)break;md.update(buf,0,n)} }; return md.digest().joinToString(""){"%02x".format(it)} }
}
