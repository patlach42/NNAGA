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

import android.view.Surface

/** JNI facade for the in-process ysfx graphics host. */
object JsfxBridge {
    external fun nativeHasGfx(pathId: Long, pluginInstanceId: Long): Boolean

    external fun nativeAttach(
        pathId: Long,
        pluginInstanceId: Long,
        surface: Surface,
        width: Int,
        height: Int,
    ): Boolean

    external fun nativeDetach(pathId: Long, pluginInstanceId: Long)
    external fun nativeSetFocus(pathId: Long, pluginInstanceId: Long, focused: Boolean)
    external fun nativeResize(pathId: Long, pluginInstanceId: Long, width: Int, height: Int)
    external fun nativeSetVisible(pathId: Long, pluginInstanceId: Long, visible: Boolean)

    external fun nativePointer(
        pathId: Long,
        pluginInstanceId: Long,
        action: Int,
        pointerId: Int,
        x: Float,
        y: Float,
        buttons: Int,
        scrollX: Float,
        scrollY: Float,
        modifiers: Int,
    )

    external fun nativeKey(
        pathId: Long,
        pluginInstanceId: Long,
        down: Boolean,
        keyCode: Int,
        unicode: Int,
        modifiers: Int,
        repeat: Int,
    )

    external fun nativeSetDropFiles(pathId: Long, pluginInstanceId: Long, paths: Array<String>)

    /** Returns one pending request as {"id": Long, "spec": String, "x": Int, "y": Int}, or null. */
    external fun nativePollMenu(pathId: Long, pluginInstanceId: Long): String?
    external fun nativeRespondMenu(pathId: Long, pluginInstanceId: Long, requestId: Long, itemId: Int)
    external fun nativeGetCursor(pathId: Long, pluginInstanceId: Long): Int


    /** Returns preferred width in the high 32 bits and height in the low 32 bits. */
    external fun nativeGetPreferredSize(pathId: Long, pluginInstanceId: Long): Long
}
