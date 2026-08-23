package com.vibes.dsp.ui

import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver

/** Formats accumulated musical quarter notes on the canonical 960-subdivision grid. */
fun formatMusicalPosition(musicalQuarterNotes: Double): String {
    val units = (musicalQuarterNotes.coerceAtLeast(0.0) * 960.0).toLong()
    val sixteenths = units / 240L
    val subdivision = units % 240L
    val bar = sixteenths / 16L + 1L
    val beat = sixteenths / 4L % 4L + 1L
    val sixteenth = sixteenths % 4L + 1L
    return "$bar:$beat:$sixteenth:$subdivision"
}

/** Returns display-only extrapolation from a captured native snapshot. */
fun interpolatedMusicalQuarterNotes(
    musicalQuarterNotes: Double,
    beatsPerMinute: Double,
    playing: Boolean,
    capturedAtMonotonicNanos: Long,
    nowMonotonicNanos: Long,
): Double {
    if (!playing || capturedAtMonotonicNanos <= 0L) return musicalQuarterNotes
    val elapsedSeconds = ((nowMonotonicNanos - capturedAtMonotonicNanos).coerceAtLeast(0L)) / 1_000_000_000.0
    return musicalQuarterNotes + elapsedSeconds * beatsPerMinute.coerceAtLeast(0.0) / 60.0
}
/** Advances on the Compose frame clock only while playback is visible and lifecycle-started. */
@Composable
fun rememberFrameClockNanos(active: Boolean): Long {
    val lifecycleOwner = LocalLifecycleOwner.current
    var lifecycleStarted by remember(lifecycleOwner) {
        mutableStateOf(lifecycleOwner.lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED))
    }
    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, _ ->
            lifecycleStarted = lifecycleOwner.lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }

    var frameNanos by remember { mutableLongStateOf(0L) }
    LaunchedEffect(active, lifecycleStarted) {
        if (!active || !lifecycleStarted) {
            frameNanos = 0L
            return@LaunchedEffect
        }
        while (true) {
            withFrameNanos { frameNanos = it }
        }
    }
    return frameNanos
}

/** Returns display-only elapsed-time extrapolation from the same captured snapshot. */
fun interpolatedElapsedSeconds(
    elapsedSeconds: Double,
    playing: Boolean,
    capturedAtMonotonicNanos: Long,
    nowMonotonicNanos: Long,
): Double {
    if (!playing || capturedAtMonotonicNanos <= 0L || nowMonotonicNanos <= 0L) return elapsedSeconds
    val deltaSeconds =
        ((nowMonotonicNanos - capturedAtMonotonicNanos).coerceAtLeast(0L)) / 1_000_000_000.0
    return elapsedSeconds + deltaSeconds
}
