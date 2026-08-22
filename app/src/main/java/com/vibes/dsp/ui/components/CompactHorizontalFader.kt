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

package com.vibes.dsp.ui.components

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.focusable
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.semantics.ProgressBarRangeInfo
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.disabled
import androidx.compose.ui.semantics.progressBarRangeInfo
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.setProgress
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.unit.dp

/** Horizontal counterpart of NNAGA's mixer fader: thin rail and rectangular thumb. */
@Composable
fun CompactHorizontalFader(
    value: Float,
    onValueChange: (Float) -> Unit,
    valueRange: ClosedFloatingPointRange<Float> = 0f..1f,
    label: String,
    valueStateDescription: String? = null,
    modifier: Modifier = Modifier,
    railVerticalPosition: Float = 0.5f,
    enabled: Boolean = true,
    onValueChangeFinished: () -> Unit = {},
) {
    val startValue = valueRange.start
    val endValue = valueRange.endInclusive
    val safeValue = value.coerceIn(startValue, endValue)
    val span = endValue - startValue
    val level = if (span > 0f) (safeValue - startValue) / span else 0f
    val accent = MaterialTheme.colorScheme.primary
    val rail = MaterialTheme.colorScheme.outlineVariant
    val disabledColor = MaterialTheme.colorScheme.onSurfaceVariant
    val currentOnValueChange by rememberUpdatedState(onValueChange)
    val currentOnValueChangeFinished by rememberUpdatedState(onValueChangeFinished)

    fun valueAt(fraction: Float): Float = startValue + span * fraction.coerceIn(0f, 1f)

    Canvas(
        modifier = modifier
            .semantics {
                contentDescription = label
                valueStateDescription?.let { stateDescription = it }
                if (!enabled) disabled()
                progressBarRangeInfo = ProgressBarRangeInfo(safeValue, valueRange)
                setProgress { target ->
                    if (!enabled) {
                        false
                    } else {
                        currentOnValueChange(target.coerceIn(startValue, endValue))
                        currentOnValueChangeFinished()
                        true
                    }
                }
            }
            .onKeyEvent { event ->
                if (!enabled || event.type != KeyEventType.KeyDown || span <= 0f) {
                    false
                } else {
                    val step = span / 100f
                    val target = when (event.key) {
                        Key.DirectionLeft, Key.DirectionDown -> safeValue - step
                        Key.DirectionRight, Key.DirectionUp -> safeValue + step
                        Key.MoveHome -> startValue
                        Key.MoveEnd -> endValue
                        else -> return@onKeyEvent false
                    }
                    currentOnValueChange(target.coerceIn(startValue, endValue))
                    currentOnValueChangeFinished()
                    true
                }
            }
            .focusable(enabled)
            .pointerInput(enabled, valueRange) {
                if (enabled) {
                    detectTapGestures { position ->
                        val inset = 8.dp.toPx()
                        val width = size.width - inset * 2f
                        val fraction = if (width > 0f) (position.x - inset) / width else 0f
                        currentOnValueChange(valueAt(fraction))
                        currentOnValueChangeFinished()
                    }
                }
            }
            .pointerInput(enabled, valueRange) {
                if (enabled) {
                    detectHorizontalDragGestures(
                        onDragStart = { position ->
                            val inset = 8.dp.toPx()
                            val width = size.width - inset * 2f
                            val fraction = if (width > 0f) (position.x - inset) / width else 0f
                            currentOnValueChange(valueAt(fraction))
                        },
                        onDragEnd = currentOnValueChangeFinished,
                        onDragCancel = currentOnValueChangeFinished,
                        onHorizontalDrag = { change, _ ->
                            change.consume()
                            val inset = 8.dp.toPx()
                            val width = size.width - inset * 2f
                            val fraction = if (width > 0f) (change.position.x - inset) / width else 0f
                            currentOnValueChange(valueAt(fraction))
                        },
                    )
                }
            },
    ) {
        val inset = 8.dp.toPx()
        val railStart = inset
        val railEnd = size.width - inset
        val railY = size.height * railVerticalPosition.coerceIn(0f, 1f)
        val thumbX = railStart + (railEnd - railStart) * level.coerceIn(0f, 1f)
        drawLine(
            color = rail,
            start = Offset(railStart, railY),
            end = Offset(railEnd, railY),
            strokeWidth = 2.dp.toPx(),
            cap = StrokeCap.Round,
        )
        drawLine(
            color = if (enabled) accent else disabledColor,
            start = Offset(railStart, railY),
            end = Offset(thumbX, railY),
            strokeWidth = 2.dp.toPx(),
            cap = StrokeCap.Round,
        )
        drawRect(
            color = if (enabled) accent else disabledColor,
            topLeft = Offset(thumbX - 2.dp.toPx(), railY - 9.dp.toPx()),
            size = Size(4.dp.toPx(), 18.dp.toPx()),
        )
    }
}
