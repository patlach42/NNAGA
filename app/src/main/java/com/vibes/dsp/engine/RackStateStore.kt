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

import android.content.Context
import androidx.core.util.AtomicFile
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import java.io.File
import java.io.FileNotFoundException

/** Atomic, bounded persistence for the native versioned rack-state blob. */
class RackStateStore(context: Context) {
    companion object {
        private const val FILE_NAME = "rack-state-v1.bin"
        private const val QUARANTINE_PREFIX = "rack-state-v1.corrupt-"
        const val MAX_BYTES = 16 * 1024 * 1024
    }

    private val file = File(context.applicationContext.filesDir, FILE_NAME)
    private val atomicFile = AtomicFile(file)
    private val writeMutex = Mutex()

    suspend fun save(bytes: ByteArray) = writeMutex.withLock {
        require(bytes.isNotEmpty()) { "native rack state is empty" }
        require(bytes.size <= MAX_BYTES) { "native rack state exceeds $MAX_BYTES bytes" }
        val output = atomicFile.startWrite()
        try {
            output.write(bytes)
            atomicFile.finishWrite(output)
        } catch (t: Throwable) {
            atomicFile.failWrite(output)
            throw t
        }
    }

    /** Returns null when no state exists. Reads are capped before allocation. */
    fun load(): ByteArray? {
        val input = try {
            atomicFile.openRead()
        } catch (_: FileNotFoundException) {
            return null
        }
        return try {
            input.use {
                val result = ByteArray(MAX_BYTES + 1)
                var count = 0
                while (count < result.size) {
                    val read = it.read(result, count, result.size - count)
                    if (read < 0) break
                    if (read == 0) continue
                    count += read
                }
                if (count > MAX_BYTES) {
                    throw IllegalArgumentException("rack state exceeds $MAX_BYTES bytes")
                }
                if (count == 0) throw IllegalArgumentException("rack state is empty")
                result.copyOf(count)
            }
        } catch (t: Throwable) {
            quarantine(t.message ?: "read failed")
            null
        }
    }

    /** Preserve a rejected blob for diagnosis; never replace it with a blank state. */
    fun quarantine(reason: String) {
        if (!file.exists()) return
        val suffix = "${System.currentTimeMillis()}-${reason.hashCode().toUInt().toString(16)}"
        val target = File(file.parentFile, QUARANTINE_PREFIX + suffix)
        if (!file.renameTo(target)) {
            android.util.Log.w("RackStateStore", "Could not quarantine rejected rack state")
        } else {
            android.util.Log.w("RackStateStore", "Quarantined rejected rack state: ${target.name}")
        }
    }
}
