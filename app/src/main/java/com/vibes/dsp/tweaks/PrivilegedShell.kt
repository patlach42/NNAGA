/*
 * Copyright (C) 2026 patlach42
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

package com.vibes.dsp.tweaks

import android.util.Log
import java.io.BufferedReader
import java.io.InputStreamReader
import java.util.concurrent.TimeUnit

/**
 * Runs a command with more privilege than the app has, when something on the
 * device is willing to grant it.
 *
 * Root here is not a system-wide `su` binary: the reference device uses
 * KernelSU, which grants root per application rather than placing su on the
 * path, so an adb shell sees nothing while a permitted app can exec it. That
 * is why availability is decided by trying, once, rather than by looking for a
 * file.
 *
 * Every call is bounded and every failure is a value, not an exception. A
 * tweak that cannot be applied must leave the app running exactly as it was.
 */
object PrivilegedShell {
    private const val TAG = "PrivilegedShell"
    private const val TIMEOUT_SECONDS = 8L

    data class Result(
        val ok: Boolean,
        val stdout: String = "",
        val stderr: String = "",
    ) {
        val output: String get() = if (stdout.isNotBlank()) stdout else stderr
    }

    enum class Access {
        /** Root, via whatever manager the device uses. */
        Root,

        /** No elevation available; only what the app itself may do. */
        None,
    }

    @Volatile
    private var cachedAccess: Access? = null

    /**
     * Whether elevation is available. Probed once and remembered: asking costs
     * a process launch, and on a manager that prompts, asking repeatedly would
     * pester the user.
     */
    fun access(): Access = cachedAccess ?: probe().also { cachedAccess = it }

    /** Forget the probe, so a permission granted since is picked up. */
    fun invalidate() {
        cachedAccess = null
    }

    private fun probe(): Access {
        val result = runAsRoot("id -u")
        // Trust the answer, not the exit code: some managers return success
        // for a denied request.
        return if (result.ok && result.stdout.trim() == "0") Access.Root else Access.None
    }

    /** Runs [command] as root. Never throws; a refusal is an unsuccessful result. */
    fun runAsRoot(command: String): Result = exec(arrayOf("su", "-c", command))

    /**
     * Reads a single sysfs or procfs value. Returns null when unreadable,
     * which is the normal case for most of these paths without elevation.
     */
    fun readPrivileged(path: String): String? {
        val result = runAsRoot("cat '$path' 2>/dev/null")
        return result.stdout.trim().takeIf { result.ok && it.isNotEmpty() }
    }

    /**
     * Writes a sysfs value and reads it back, because a write to sysfs can
     * succeed and be ignored: the kernel or a vendor daemon may clamp or
     * revert it. Returns what the file holds afterwards.
     */
    fun writePrivilegedAndVerify(path: String, value: String): Result {
        val write = runAsRoot("echo '$value' > '$path'")
        if (!write.ok) return write
        val readBack = readPrivileged(path)
            ?: return Result(false, stderr = "wrote but could not read back")
        return Result(
            ok = readBack.trim() == value.trim(),
            stdout = readBack,
            stderr = if (readBack.trim() == value.trim()) {
                ""
            } else {
                "value did not stick: wrote $value, read $readBack"
            },
        )
    }

    private fun exec(command: Array<String>): Result = try {
        val process = ProcessBuilder(*command).redirectErrorStream(false).start()
        val out = StringBuilder()
        val err = StringBuilder()
        BufferedReader(InputStreamReader(process.inputStream)).use { reader ->
            reader.forEachLine { out.appendLine(it) }
        }
        BufferedReader(InputStreamReader(process.errorStream)).use { reader ->
            reader.forEachLine { err.appendLine(it) }
        }
        val finished = process.waitFor(TIMEOUT_SECONDS, TimeUnit.SECONDS)
        if (!finished) {
            process.destroyForcibly()
            Result(false, stderr = "timed out after ${TIMEOUT_SECONDS}s")
        } else {
            Result(process.exitValue() == 0, out.toString(), err.toString())
        }
    } catch (t: Throwable) {
        Log.d(TAG, "exec failed: ${t.message}")
        Result(false, stderr = t.message ?: t.javaClass.simpleName)
    }
}
