package com.vibes.dsp.ui.components

import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.MaterialTheme
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.compose.foundation.layout.padding
import kotlin.math.floor
import kotlin.math.roundToInt

/** Musical position represented as bars:beats:sixteenths:subdivision. */
object MusicalPosition {
    const val SubdivisionsPerQuarter = 960
    const val SubdivisionsPerSixteenth = SubdivisionsPerQuarter / 4
    const val SixteenthQuarterNotes = 0.25

    fun format(quarterNotes: Double): String {
        val units = (quarterNotes.coerceAtLeast(0.0) * SubdivisionsPerQuarter).roundToInt()
        val sixteenths = units / SubdivisionsPerSixteenth
        val subdivision = units % SubdivisionsPerSixteenth
        val bar = sixteenths / 16 + 1
        val beat = (sixteenths / 4) % 4 + 1
        val sixteenth = sixteenths % 4 + 1
        return "$bar:$beat:$sixteenth:$subdivision"
    }

    /** Null indicates malformed, non-finite, or negative input. */
    fun parse(text: String): Double? {
        val parts = text.trim().split(':')
        if (parts.size != 4) return null
        val numbers = parts.map { it.toLongOrNull() ?: return null }
        val bar = numbers[0]
        val beat = numbers[1]
        val sixteenth = numbers[2]
        val subdivision = numbers[3]
        if (bar < 1L || beat !in 1L..4L || sixteenth !in 1L..4L ||
            subdivision !in 0L until SubdivisionsPerSixteenth.toLong()) return null
        val sixteenthIndex = try {
            Math.addExact(Math.multiplyExact(bar - 1L, 16L),
                Math.addExact(Math.multiplyExact(beat - 1L, 4L), sixteenth - 1L))
        } catch (_: ArithmeticException) {
            return null
        }
        val units = try {
            Math.addExact(Math.multiplyExact(sixteenthIndex, SubdivisionsPerSixteenth.toLong()), subdivision)
        } catch (_: ArithmeticException) {
            return null
        }
        return units.toDouble() / SubdivisionsPerQuarter
    }
}

/**
 * Text + gesture editor. Native/ViewModel is called only after a valid, atomic commit.
 */
@Composable
fun MusicalPositionControl(
    label: String,
    quarterNotes: Double,
    maxQuarterNotes: Double,
    onCommit: (Double, (Boolean) -> Unit) -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
) {
    var dragRemainder by remember { mutableStateOf(0.0) }
    var text by remember { mutableStateOf(MusicalPosition.format(quarterNotes)) }
    var error by remember { mutableStateOf<String?>(null) }
    var lastCommitted by remember { mutableStateOf(quarterNotes) }
    LaunchedEffect(quarterNotes) {
        lastCommitted = quarterNotes
        text = MusicalPosition.format(quarterNotes)
        error = null
    }
    fun commit(candidate: String) {
        val parsed = MusicalPosition.parse(candidate)
        if (parsed == null) {
            error = "Use bar:beat:sixteenth:subdivision"
            return
        }
        if (parsed > maxQuarterNotes + 1e-9) {
            error = "Position exceeds clip duration"
            return
        }
        val clamped = parsed.coerceIn(0.0, maxQuarterNotes)
        error = null
        if (clamped != quarterNotes) {
            onCommit(clamped) { accepted ->
                if (accepted) {
                    lastCommitted = clamped
                    text = MusicalPosition.format(clamped)
                } else {
                    lastCommitted = quarterNotes
                    text = MusicalPosition.format(quarterNotes)
                    error = "Native transport rejected value"
                }
            }
        } else {
            lastCommitted = clamped
            text = MusicalPosition.format(clamped)
        }
    }
    fun adjust(delta: Double) {
        val next = (lastCommitted + delta).coerceIn(0.0, maxQuarterNotes)
        if (next != lastCommitted) {
            onCommit(next) { accepted ->
                if (accepted) {
                    lastCommitted = next
                    text = MusicalPosition.format(next)
                    error = null
                } else {
                    text = MusicalPosition.format(lastCommitted)
                    error = "Native transport rejected value"
                }
            }
        }
    }
    Column(
        modifier = modifier
            .onFocusChanged { state -> if (!state.isFocused && enabled) commit(text) }
            .pointerInput(enabled, maxQuarterNotes) {
                if (!enabled) return@pointerInput
                detectDragGestures(
                    onDragEnd = { dragRemainder = 0.0 },
                    onDragCancel = { dragRemainder = 0.0 },
                ) { change, dragAmount ->
                    change.consume()
                    val coarseQuantum = MusicalPosition.SixteenthQuarterNotes
                    val quantum = dragAmount.x / 24.dp.toPx() * coarseQuantum + dragRemainder
                    val coarseSteps = (quantum / coarseQuantum).toInt()
                    val coarse = coarseSteps * coarseQuantum
                    val fine = ((quantum - coarse) * MusicalPosition.SubdivisionsPerQuarter)
                        .toInt() / MusicalPosition.SubdivisionsPerQuarter.toDouble()
                    dragRemainder = quantum - coarse - fine
                    if (coarse + fine != 0.0) adjust(coarse + fine)
                }
            }
            .semantics {
                stateDescription = text
                customActions = if (enabled) listOf(
                    CustomAccessibilityAction("Increment $label") { adjust(MusicalPosition.SixteenthQuarterNotes); true },
                    CustomAccessibilityAction("Decrement $label") { adjust(-MusicalPosition.SixteenthQuarterNotes); true },
                ) else emptyList()
            },
    ) {
        OutlinedTextField(
            value = text,
            onValueChange = { text = it; error = null },
            label = { Text(label) },
            supportingText = { error?.let { Text(it, color = MaterialTheme.colorScheme.error) } },
            isError = error != null,
            enabled = enabled,
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
            keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
            keyboardActions = KeyboardActions(onDone = { if (enabled) commit(text) }),
        )
        if (error == null) {
            Text("Drag horizontally for 1/16-note steps", style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.padding(start = 4.dp))
        }
    }
}
