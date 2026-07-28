/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * Licensed under GPL v3 — see app/src/main/cpp/plugin/IPlugin.h for full notice.
 */

package com.vibes.dsp.ui.vst

import android.content.Context
import android.system.Os
import android.util.Log
import com.varcain.vsthost.NativeBridge
import com.varcain.vsthost.wine.WineSetup
import java.io.File
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.Path

/**
 * Bridge between :app's VST UI and :vsthost_lib's WineSetup. Responsible for:
 *
 *   1. ensureWineRoot()       — global one-time-per-version extraction of
 *                                wine binaries from jniLibs/assets into
 *                                filesDir/wine/ + filesDir/wineprefix/ +
 *                                staging of vst_host.exe variants.
 *   2. ensurePluginPrefix()    — per-imported-VST clone of the base prefix,
 *                                seeded with the same registry/DLL overrides
 *                                vstpoc's HostViewModel applies. Idempotent.
 *
 * Both are blocking, slow (seconds), and must NOT run on the main thread.
 * Callers wrap in Dispatchers.IO.
 */
object VstHostSetup {
    private const val TAG = "VstHostSetup"

    /** Lives in filesDir alongside vstpoc's existing artifacts; one stamp
     *  per imported VST keyed by uuid so we don't re-seed on every rack add. */
    private const val PREFIX_STAMP_DIR = "vst_plugin_prefixes_ready"
    private const val SERVICES_BOOT_MARKER = ".vstpoc_services_booted_v1"

    fun ensureWineRoot(context: Context): Boolean {
        return try {
            val setup = WineSetup.ensure(context)
            Log.i(TAG, "wine setup ready: root=${setup.wineRoot.absolutePath}")
            // Stage vst_host.exe variants into filesDir/. vstpoc's
            // HostViewModel.startWineVst then copies from filesDir/ into
            // filesDir/tmp/ at startWineVst time; our WineVstPlugin reads
            // from assetsDir which we point at filesDir.
            for (name in listOf("vst_host.exe", "vst_host_x86.exe", "vst3_host.exe")) {
                runCatching {
                    context.assets.open(name).use { input ->
                        File(context.filesDir, name).outputStream().use { input.copyTo(it) }
                    }
                }
            }
            // Virtual-desktop registry: required before any wine process spawns
            // so winex11.drv sizes the desktop window properly.
            WineSetup.applyVirtualDesktopRegistry(context)
            true
        } catch (t: Throwable) {
            Log.e(TAG, "WineSetup.ensure failed", t)
            false
        }
    }

    /**
     * Make sure a wineprefix exists for the given imported VST. Mirrors
     * vstpoc's HostViewModel:880-953 per-prefix seeding:
     *   - clone base wineprefix → wineprefix_v<uuid>
     *   - seed Win7 version + disable D3D + disable menubuilder
     *   - install DXVK + UIHost stub
     *   - register WinRT activatable classes + Common Controls SxS manifest
     *   - seed Program Files dirs + registry
     *
     * Idempotent — skip on subsequent calls via a stamp file. Sentinel file
     * makes it safe to call from import (where it's blocking with progress UI)
     * and from rack-add (where prefix already exists from import).
     */
    fun ensurePluginPrefix(context: Context, uuid: String): Boolean {
        val basePrefix = File(context.filesDir, "wineprefix")
        if (!basePrefix.exists()) {
            Log.e(TAG, "base wineprefix missing — call ensureWineRoot first")
            return false
        }
        val prefix = File(context.filesDir, "wineprefix_v$uuid")
        val stampDir = File(context.filesDir, PREFIX_STAMP_DIR).apply { mkdirs() }
        val stamp = File(stampDir, "$uuid.ready")
        return try {
            if (!prefix.exists()) {
                Log.i(TAG, "cloning $basePrefix → $prefix")
                copyDirectoryTree(basePrefix, prefix)
            }
            // Idempotent seeders — safe to call every time the user opens the
            // manager. Cheap on the second pass.
            WineSetup.seedWindowsVersion(prefix)
            WineSetup.seedDisableMenubuilder(prefix)
            WineSetup.seedWow64Emulator(prefix)  // wine 11.x: ARM64EC/wow64 → FEX
            // Per-plugin GPU disable: when this marker exists, skip the
            // d3d-overrides seed and DXVK install so wine's builtin d3d11
            // (via wined3d) is loaded instead. AmpliTube 5 black-render
            // workaround — DXVK 2.5.3 partially init on Adreno triggers
            // memory-alloc fail + JUCE crash. Marker file is plugin-set
            // (touched via adb or per-plugin UI affordance).
            val noDxvk = File(prefix, ".no-dxvk").exists()
            if (noDxvk) {
                Log.i(TAG, "ensurePluginPrefix($uuid): .no-dxvk marker present — skipping DXVK install + d3d overrides")
            } else {
                WineSetup.seedDisableDirect3D(prefix)
                WineSetup.installDxvk(context, prefix)
            }
            WineSetup.seedActivatableClasses(prefix)
            WineSetup.seedCommonControlsManifests(prefix)
            WineSetup.seedProgramFilesDirs(prefix)
            WineSetup.installUiHostStub(context, prefix)
            stamp.writeText(System.currentTimeMillis().toString())
            true
        } catch (t: Throwable) {
            Log.e(TAG, "ensurePluginPrefix($uuid) failed", t)
            false
        }
    }

    fun bootstrapPluginPrefixServices(context: Context, uuid: String): Boolean =
        bootstrapPrefixServices(context, File(context.filesDir, "wineprefix_v$uuid"))

    /** Prime the one-time wine service bootstrap that WineHostProcess.start()
     *  otherwise performs on first rack insertion. Failure is non-fatal: the
     *  next plugin launch will retry through the existing activation path. */
    fun bootstrapPrefixServices(context: Context, prefix: File): Boolean {
        val marker = File(prefix, SERVICES_BOOT_MARKER)
        if (marker.exists()) {
            Log.i(TAG, "bootstrapPrefixServices: marker present for ${prefix.name}, skip")
            return true
        }
        if (!prefix.exists()) {
            Log.w(TAG, "bootstrapPrefixServices: prefix missing at ${prefix.absolutePath}")
            return false
        }

        return try {
            val setup = WineSetup.ensure(context)
            val started = System.currentTimeMillis()
            val ok = NativeBridge.nativeBootstrapWineServices(
                prefixPath = prefix.absolutePath,
                wineBinary = setup.wineBinary.absolutePath,
                wineserverBinary = setup.wineServer.absolutePath,
                wineDllPath = setup.wineDllPath.absolutePath,
                nativeLibDir = setup.nativeLibraryDir.absolutePath,
                cacheDir = context.cacheDir.absolutePath,
            )
            val took = System.currentTimeMillis() - started
            val markerReady = marker.exists()
            Log.i(TAG, "bootstrapPrefixServices: prefix=${prefix.name} ok=$ok " +
                       "marker=$markerReady in ${took}ms")
            ok || markerReady
        } catch (t: Throwable) {
            Log.w(TAG, "bootstrapPrefixServices(${prefix.name}) failed; rack add will retry", t)
            false
        }
    }

    /** Public symlink-preserving copy (reused by the installer flow to
     *  clone the base prefix into a one-shot template). */
    fun copyPrefix(src: File, dst: File) = copyDirectoryTree(src, dst)

    /** Send SIGTERM to any wineserver process whose WINEPREFIX env var
     *  matches [prefixPath]. Used before re-seeding a prefix's registry —
     *  wineserver caches the registry in memory and rewrites system.reg
     *  from that cache on exit, silently undoing on-disk seed changes that
     *  landed while it was running. Killing it first ensures the next wine
     *  launch reads the fresh on-disk registry. Per-prefix surgery so we
     *  don't disturb other VST chains running against different prefixes.
     *
     *  Blocks up to 2 seconds per killed wineserver for graceful exit. */
    fun killWineserversForPrefix(prefixPath: String) {
        val procDir = File("/proc")
        val pids = procDir.listFiles { f -> f.isDirectory && f.name.all { it.isDigit() } }
            ?: return
        val targeted = mutableListOf<Int>()
        for (procPid in pids) {
            val comm = runCatching { File(procPid, "comm").readText().trim() }
                .getOrNull() ?: continue
            // Don't filter by comm name. We want to kill EVERY process in
            // this wineprefix: wineserver, the wine loader, AND any
            // Windows exe (the orphan whose comm shows up as the
            // truncated exe basename, or the full chrooted path string).
            // Electron managers like IK Multimedia Product Manager use
            // requestSingleInstanceLock() and SILENTLY EXIT if any
            // previous instance is still alive — orphan exes from prior
            // launches were blocking every subsequent attempt.
            val environ = runCatching { File(procPid, "environ").readBytes() }
                .getOrNull() ?: continue
            val envText = String(environ).split(' ')
            val winePrefix = envText.firstOrNull { it.startsWith("WINEPREFIX=") }
                ?.removePrefix("WINEPREFIX=") ?: continue
            if (winePrefix.trimEnd('/') == prefixPath.trimEnd('/')) {
                val pid = procPid.name.toIntOrNull() ?: continue
                Log.i(TAG, "killWineserversForPrefix: targeting pid=$pid for $prefixPath")
                runCatching { android.os.Process.sendSignal(pid, 15 /* SIGTERM */) }
                targeted += pid
            }
        }
        // Wait briefly for graceful exit so its on-shutdown registry flush
        // (which would otherwise stomp our subsequent seed) finishes first.
        if (targeted.isNotEmpty()) {
            for (i in 0 until 20) {  // up to ~2s
                val stillAlive = targeted.any { pid ->
                    File("/proc/$pid").exists()
                }
                if (!stillAlive) break
                Thread.sleep(100)
            }
            // Final flush check: if any are still alive, force-kill.
            for (pid in targeted) {
                if (File("/proc/$pid").exists()) {
                    Log.w(TAG, "killWineserversForPrefix: pid=$pid still alive after 2s, SIGKILL")
                    runCatching { android.os.Process.sendSignal(pid, 9 /* SIGKILL */) }
                }
            }
        }
    }

    /** Idempotent seed application on an EXISTING prefix. Called from the
     *  installer flow's confirmPicks after the template is cloned to the
     *  per-plugin prefix — the seeds (DXVK DLLs, UI host stub, Win7 spoof,
     *  Common Controls SxS manifests) need to land on the per-plugin
     *  prefix the same way ensurePluginPrefix lands them on a fresh
     *  base-cloned prefix. Safe to call on a prefix that already had
     *  these seeds applied; each helper is idempotent. */
    fun applyPluginPrefixSeeds(context: Context, prefix: File) {
        WineSetup.seedWindowsVersion(prefix)
        WineSetup.seedDisableMenubuilder(prefix)
        WineSetup.seedDefaultEnvironment(prefix)
        WineSetup.seedWow64Emulator(prefix)  // wine 11.x: ARM64EC/wow64 → FEX
        if (!File(prefix, ".no-dxvk").exists()) {
            WineSetup.seedDisableDirect3D(prefix)
            WineSetup.installDxvk(context, prefix)
        } else {
            Log.i(TAG, "applyPluginPrefixSeeds: .no-dxvk marker present — skipping DXVK install + d3d overrides")
        }
        WineSetup.seedActivatableClasses(prefix)
        WineSetup.seedCommonControlsManifests(prefix)
        WineSetup.seedProgramFilesDirs(prefix)
        WineSetup.installUiHostStub(context, prefix)
    }

    /** Recursive copy that PRESERVES SYMLINKS — critical for wineprefix
     *  clones because dosdevices/c: and dosdevices/z: are symlinks
     *  (-> ../drive_c, -> /) and wine's path resolution requires them as
     *  symlinks. The naive File.isDirectory() check follows symlinks, so
     *  copying it as a regular tree dereferences c: to drive_c (good) and
     *  z: to / (catastrophic — tries to copy the entire filesystem). */
    private fun copyDirectoryTree(src: File, dst: File) {
        dst.mkdirs()
        src.listFiles()?.forEach { entry ->
            // .wineserver is pure runtime state (a unix-domain socket + lock);
            // never clone it — opening the socket as a regular file throws ENXIO
            // ("No such device or address"), and a fresh wineserver is created on
            // the clone's first launch anyway.
            if (entry.name == ".wineserver") return@forEach
            val target = File(dst, entry.name)
            val isSymlink = Files.isSymbolicLink(entry.toPath())
            when {
                isSymlink -> {
                    // Read the symlink target and recreate as a symlink.
                    val link: Path = entry.toPath()
                    val linkTarget = Files.readSymbolicLink(link).toString()
                    target.delete()
                    Os.symlink(linkTarget, target.absolutePath)
                }
                Files.isDirectory(entry.toPath(), LinkOption.NOFOLLOW_LINKS) -> {
                    copyDirectoryTree(entry, target)
                }
                Files.isRegularFile(entry.toPath(), LinkOption.NOFOLLOW_LINKS) -> {
                    entry.inputStream().use { input ->
                        target.outputStream().use { output -> input.copyTo(output) }
                    }
                }
                // else: socket / FIFO / device node — runtime state, not data; skip.
            }
        }
    }
}
