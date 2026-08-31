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

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.size
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.vibes.dsp.R

/** Complete Fontaudio 1.1 private-use glyph catalog (U+F101..U+F19B). */
enum class FontaudioGlyph(val text: String) {
    /** Upstream glyph: fad-ADR. */
    ADR("\uF101"),
    /** Upstream glyph: fad-ADSR. */
    ADSR("\uF102"),
    /** Upstream glyph: fad-AHDSR. */
    AHDSR("\uF103"),
    /** Upstream glyph: fad-AR. */
    AR("\uF104"),
    /** Upstream glyph: fad-armrecording. */
    ARM_RECORDING("\uF105"),
    /** Upstream glyph: fad-arpchord. */
    ARP_CHORD("\uF106"),
    /** Upstream glyph: fad-arpdown. */
    ARP_DOWN("\uF107"),
    /** Upstream glyph: fad-arpdownandup. */
    ARP_DOWN_AND_UP("\uF108"),
    /** Upstream glyph: fad-arpdownup. */
    ARP_DOWN_UP("\uF109"),
    /** Upstream glyph: fad-arpplayorder. */
    ARP_PLAY_ORDER("\uF10A"),
    /** Upstream glyph: fad-arprandom. */
    ARP_RANDOM("\uF10B"),
    /** Upstream glyph: fad-arpup. */
    ARP_UP("\uF10C"),
    /** Upstream glyph: fad-arpupandown. */
    ARP_UP_AND_DOWN("\uF10D"),
    /** Upstream glyph: fad-arpupdown. */
    ARP_UP_DOWN("\uF10E"),
    /** Upstream glyph: fad-arrows-horz. */
    ARROWS_HORIZONTAL("\uF10F"),
    /** Upstream glyph: fad-arrows-vert. */
    ARROWS_VERTICAL("\uF110"),
    /** Upstream glyph: fad-automation-2p. */
    AUTOMATION_2P("\uF111"),
    /** Upstream glyph: fad-automation-3p. */
    AUTOMATION_3P("\uF112"),
    /** Upstream glyph: fad-automation-4p. */
    AUTOMATION_4P("\uF113"),
    /** Upstream glyph: fad-backward. */
    BACKWARD("\uF114"),
    /** Upstream glyph: fad-bluetooth. */
    BLUETOOTH("\uF115"),
    /** Upstream glyph: fad-caret-down. */
    CARET_DOWN("\uF116"),
    /** Upstream glyph: fad-caret-left. */
    CARET_LEFT("\uF117"),
    /** Upstream glyph: fad-caret-right. */
    CARET_RIGHT("\uF118"),
    /** Upstream glyph: fad-caret-up. */
    CARET_UP("\uF119"),
    /** Upstream glyph: fad-close. */
    CLOSE("\uF11A"),
    /** Upstream glyph: fad-copy. */
    COPY("\uF11B"),
    /** Upstream glyph: fad-cpu. */
    CPU("\uF11C"),
    /** Upstream glyph: fad-cutter. */
    CUTTER("\uF11D"),
    /** Upstream glyph: fad-digital-colon. */
    DIGITAL_COLON("\uF11E"),
    /** Upstream glyph: fad-digital-dot. */
    DIGITAL_DOT("\uF11F"),
    /** Upstream glyph: fad-digital0. */
    DIGITAL_0("\uF120"),
    /** Upstream glyph: fad-digital1. */
    DIGITAL_1("\uF121"),
    /** Upstream glyph: fad-digital2. */
    DIGITAL_2("\uF122"),
    /** Upstream glyph: fad-digital3. */
    DIGITAL_3("\uF123"),
    /** Upstream glyph: fad-digital4. */
    DIGITAL_4("\uF124"),
    /** Upstream glyph: fad-digital5. */
    DIGITAL_5("\uF125"),
    /** Upstream glyph: fad-digital6. */
    DIGITAL_6("\uF126"),
    /** Upstream glyph: fad-digital7. */
    DIGITAL_7("\uF127"),
    /** Upstream glyph: fad-digital8. */
    DIGITAL_8("\uF128"),
    /** Upstream glyph: fad-digital9. */
    DIGITAL_9("\uF129"),
    /** Upstream glyph: fad-diskio. */
    DISK_IO("\uF12A"),
    /** Upstream glyph: fad-drumpad. */
    DRUM_PAD("\uF12B"),
    /** Upstream glyph: fad-duplicate. */
    DUPLICATE("\uF12C"),
    /** Upstream glyph: fad-eraser. */
    ERASER("\uF12D"),
    /** Upstream glyph: fad-ffwd. */
    FAST_FORWARD("\uF12E"),
    /** Upstream glyph: fad-filter-bandpass. */
    FILTER_BANDPASS("\uF12F"),
    /** Upstream glyph: fad-filter-bell. */
    FILTER_BELL("\uF130"),
    /** Upstream glyph: fad-filter-bypass. */
    FILTER_BYPASS("\uF131"),
    /** Upstream glyph: fad-filter-highpass. */
    FILTER_HIGHPASS("\uF132"),
    /** Upstream glyph: fad-filter-lowpass. */
    FILTER_LOWPASS("\uF133"),
    /** Upstream glyph: fad-filter-notch. */
    FILTER_NOTCH("\uF134"),
    /** Upstream glyph: fad-filter-rez-highpass. */
    FILTER_RESONANCE_HIGHPASS("\uF135"),
    /** Upstream glyph: fad-filter-rez-lowpass. */
    FILTER_RESONANCE_LOWPASS("\uF136"),
    /** Upstream glyph: fad-filter-shelving-hi. */
    FILTER_SHELVING_HIGH("\uF137"),
    /** Upstream glyph: fad-filter-shelving-lo. */
    FILTER_SHELVING_LOW("\uF138"),
    /** Upstream glyph: fad-foldback. */
    FOLDBACK("\uF139"),
    /** Upstream glyph: fad-forward. */
    FORWARD("\uF13A"),
    /** Upstream glyph: fad-h-expand. */
    HORIZONTAL_EXPAND("\uF13B"),
    /** Upstream glyph: fad-hardclip. */
    HARD_CLIP("\uF13C"),
    /** Upstream glyph: fad-hardclipcurve. */
    HARD_CLIP_CURVE("\uF13D"),
    /** Upstream glyph: fad-headphones. */
    HEADPHONES("\uF13E"),
    /** Upstream glyph: fad-keyboard. */
    KEYBOARD("\uF13F"),
    /** Upstream glyph: fad-lock. */
    LOCK("\uF140"),
    /** Upstream glyph: fad-logo-aax. */
    LOGO_AAX("\uF141"),
    /** Upstream glyph: fad-logo-abletonlink. */
    LOGO_ABLETONLINK("\uF142"),
    /** Upstream glyph: fad-logo-au. */
    LOGO_AU("\uF143"),
    /** Upstream glyph: fad-logo-audacity. */
    LOGO_AUDACITY("\uF144"),
    /** Upstream glyph: fad-logo-audiobus. */
    LOGO_AUDIOBUS("\uF145"),
    /** Upstream glyph: fad-logo-cubase. */
    LOGO_CUBASE("\uF146"),
    /** Upstream glyph: fad-logo-fl. */
    LOGO_FL("\uF147"),
    /** Upstream glyph: fad-logo-juce. */
    LOGO_JUCE("\uF148"),
    /** Upstream glyph: fad-logo-ladspa. */
    LOGO_LADSPA("\uF149"),
    /** Upstream glyph: fad-logo-live. */
    LOGO_LIVE("\uF14A"),
    /** Upstream glyph: fad-logo-lv2. */
    LOGO_LV_2("\uF14B"),
    /** Upstream glyph: fad-logo-protools. */
    LOGO_PROTOOLS("\uF14C"),
    /** Upstream glyph: fad-logo-rackext. */
    LOGO_RACKEXT("\uF14D"),
    /** Upstream glyph: fad-logo-reaper. */
    LOGO_REAPER("\uF14E"),
    /** Upstream glyph: fad-logo-reason. */
    LOGO_REASON("\uF14F"),
    /** Upstream glyph: fad-logo-rewire. */
    LOGO_REWIRE("\uF150"),
    /** Upstream glyph: fad-logo-studioone. */
    LOGO_STUDIOONE("\uF151"),
    /** Upstream glyph: fad-logo-tracktion. */
    LOGO_TRACKTION("\uF152"),
    /** Upstream glyph: fad-logo-vst. */
    LOGO_VST("\uF153"),
    /** Upstream glyph: fad-logo-waveform. */
    LOGO_WAVEFORM("\uF154"),
    /** Upstream glyph: fad-loop. */
    LOOP("\uF155"),
    /** Upstream glyph: fad-metronome. */
    METRONOME("\uF156"),
    /** Upstream glyph: fad-microphone. */
    MICROPHONE("\uF157"),
    /** Upstream glyph: fad-midiplug. */
    MIDI_PLUG("\uF158"),
    /** Upstream glyph: fad-modrandom. */
    MOD_RANDOM("\uF159"),
    /** Upstream glyph: fad-modsawdown. */
    MOD_SAW_DOWN("\uF15A"),
    /** Upstream glyph: fad-modsawup. */
    MOD_SAW_UP("\uF15B"),
    /** Upstream glyph: fad-modsh. */
    MOD_SAMPLE_HOLD("\uF15C"),
    /** Upstream glyph: fad-modsine. */
    MOD_SINE("\uF15D"),
    /** Upstream glyph: fad-modsquare. */
    MOD_SQUARE("\uF15E"),
    /** Upstream glyph: fad-modtri. */
    MOD_TRIANGLE("\uF15F"),
    /** Upstream glyph: fad-modularplug. */
    MODULAR_PLUG("\uF160"),
    /** Upstream glyph: fad-mono. */
    MONO("\uF161"),
    /** Upstream glyph: fad-mute. */
    MUTE("\uF162"),
    /** Upstream glyph: fad-next. */
    NEXT("\uF163"),
    /** Upstream glyph: fad-open. */
    OPEN("\uF164"),
    /** Upstream glyph: fad-paste. */
    PASTE("\uF165"),
    /** Upstream glyph: fad-pause. */
    PAUSE("\uF166"),
    /** Upstream glyph: fad-pen. */
    PEN("\uF167"),
    /** Upstream glyph: fad-phase. */
    PHASE("\uF168"),
    /** Upstream glyph: fad-play. */
    PLAY("\uF169"),
    /** Upstream glyph: fad-pointer. */
    POINTER("\uF16A"),
    /** Upstream glyph: fad-powerswitch. */
    POWER_SWITCH("\uF16B"),
    /** Upstream glyph: fad-preset-a. */
    PRESET_A("\uF16C"),
    /** Upstream glyph: fad-preset-ab. */
    PRESET_AB("\uF16D"),
    /** Upstream glyph: fad-preset-b. */
    PRESET_B("\uF16E"),
    /** Upstream glyph: fad-preset-ba. */
    PRESET_BA("\uF16F"),
    /** Upstream glyph: fad-prev. */
    PREV("\uF170"),
    /** Upstream glyph: fad-punch-in. */
    PUNCH_IN("\uF171"),
    /** Upstream glyph: fad-punch-out. */
    PUNCH_OUT("\uF172"),
    /** Upstream glyph: fad-ram. */
    RAM("\uF173"),
    /** Upstream glyph: fad-random-1dice. */
    RANDOM_ONE_DIE("\uF174"),
    /** Upstream glyph: fad-random-2dice. */
    RANDOM_TWO_DICE("\uF175"),
    /** Upstream glyph: fad-record. */
    RECORD("\uF176"),
    /** Upstream glyph: fad-redo. */
    REDO("\uF177"),
    /** Upstream glyph: fad-repeat-one. */
    REPEAT_ONE("\uF178"),
    /** Upstream glyph: fad-repeat. */
    REPEAT("\uF179"),
    /** Upstream glyph: fad-rew. */
    REWIND("\uF17A"),
    /** Upstream glyph: fad-roundswitch-off. */
    ROUND_SWITCH_OFF("\uF17B"),
    /** Upstream glyph: fad-roundswitch-on. */
    ROUND_SWITCH_ON("\uF17C"),
    /** Upstream glyph: fad-save. */
    SAVE("\uF17D"),
    /** Upstream glyph: fad-saveas. */
    SAVE_AS("\uF17E"),
    /** Upstream glyph: fad-scissors. */
    SCISSORS("\uF17F"),
    /** Upstream glyph: fad-shuffle. */
    SHUFFLE("\uF180"),
    /** Upstream glyph: fad-slider-round-1. */
    SLIDER_ROUND_1("\uF181"),
    /** Upstream glyph: fad-slider-round-2. */
    SLIDER_ROUND_2("\uF182"),
    /** Upstream glyph: fad-slider-round-3. */
    SLIDER_ROUND_3("\uF183"),
    /** Upstream glyph: fad-sliderhandle-1. */
    SLIDER_HANDLE_1("\uF184"),
    /** Upstream glyph: fad-sliderhandle-2. */
    SLIDER_HANDLE_2("\uF185"),
    /** Upstream glyph: fad-softclip. */
    SOFT_CLIP("\uF186"),
    /** Upstream glyph: fad-softclipcurve. */
    SOFT_CLIP_CURVE("\uF187"),
    /** Upstream glyph: fad-solo. */
    SOLO("\uF188"),
    /** Upstream glyph: fad-speaker. */
    SPEAKER("\uF189"),
    /** Upstream glyph: fad-squareswitch-off. */
    SQUARE_SWITCH_OFF("\uF18A"),
    /** Upstream glyph: fad-squareswitch-on. */
    SQUARE_SWITCH_ON("\uF18B"),
    /** Upstream glyph: fad-stereo. */
    STEREO("\uF18C"),
    /** Upstream glyph: fad-stop. */
    STOP("\uF18D"),
    /** Upstream glyph: fad-thunderbolt. */
    THUNDERBOLT("\uF18E"),
    /** Upstream glyph: fad-timeselect. */
    TIME_SELECT("\uF18F"),
    /** Upstream glyph: fad-undo. */
    UNDO("\uF190"),
    /** Upstream glyph: fad-unlock. */
    UNLOCK("\uF191"),
    /** Upstream glyph: fad-usb. */
    USB("\uF192"),
    /** Upstream glyph: fad-v-expand. */
    VERTICAL_EXPAND("\uF193"),
    /** Upstream glyph: fad-vroundswitch-off. */
    VERTICAL_ROUND_SWITCH_OFF("\uF194"),
    /** Upstream glyph: fad-vroundswitch-on. */
    VERTICAL_ROUND_SWITCH_ON("\uF195"),
    /** Upstream glyph: fad-vsquareswitch-off. */
    VERTICAL_SQUARE_SWITCH_OFF("\uF196"),
    /** Upstream glyph: fad-vsquareswitch-on. */
    VERTICAL_SQUARE_SWITCH_ON("\uF197"),
    /** Upstream glyph: fad-waveform. */
    WAVEFORM("\uF198"),
    /** Upstream glyph: fad-xlrplug. */
    XLR_PLUG("\uF199"),
    /** Upstream glyph: fad-zoomin. */
    ZOOM_IN("\uF19A"),
    /** Upstream glyph: fad-zoomout. */
    ZOOM_OUT("\uF19B"),
}

private val FontaudioFamily = FontFamily(Font(R.font.fontaudio))

/** Renders a Fontaudio glyph without exposing its private-use character to accessibility. */
@Composable
fun FontaudioIcon(
    glyph: FontaudioGlyph,
    contentDescription: String?,
    modifier: Modifier = Modifier,
    tint: Color = LocalContentColor.current,
    size: Dp = 24.dp,
) {
    Box(
        modifier = modifier.size(size),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text = glyph.text,
            modifier = Modifier.clearAndSetSemantics {
                if (contentDescription != null) this.contentDescription = contentDescription
            },
            color = tint,
            fontFamily = FontaudioFamily,
            fontSize = with(LocalDensity.current) { size.toSp() },
            maxLines = 1,
            softWrap = false,
        )
    }
}
