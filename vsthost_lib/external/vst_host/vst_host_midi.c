#include "vst_host_midi.h"
#include <string.h>

void vst_host_midi_clear(VstHostMidiBuffer* b) {
    if (!b) return;
    b->count = 0;
    b->payload_size = 0;
    b->dropped = 0;
}

int vst_host_midi_append(VstHostMidiBuffer* b, uint32_t frame_offset,
                         const uint8_t* payload, uint32_t payload_size) {
    if (!b || !payload || payload_size == 0 || payload_size > VST_HOST_MIDI_MAX_PAYLOAD ||
        b->count >= VST_HOST_MIDI_MAX_EVENTS ||
        payload_size > VST_HOST_MIDI_MAX_PAYLOAD - b->payload_size) {
        if (b) b->dropped++;
        return 0;
    }
    VstHostMidiDesc* d = &b->events[b->count++];
    d->frame_offset = frame_offset;
    d->payload_offset = b->payload_size;
    d->payload_size = payload_size;
    memcpy(b->payload + b->payload_size, payload, payload_size);
    b->payload_size += payload_size;
    return 1;
}

int vst_host_midi_copy(VstHostMidiBuffer* dst, const VstHostMidiBuffer* src) {
    if (!dst || !src) return 0;
    vst_host_midi_clear(dst);
    for (uint32_t i = 0; i < src->count; ++i) {
        const VstHostMidiDesc* d = &src->events[i];
        if (d->payload_offset > src->payload_size ||
            d->payload_size > src->payload_size - d->payload_offset ||
            !vst_host_midi_append(dst, d->frame_offset,
                                  src->payload + d->payload_offset, d->payload_size))
            return 0;
    }
    return 1;
}

/* Frame range is validated against the block by the host; helper rejects negative. */

int vst_host_midi_append_vst_event(VstHostMidiBuffer* dst, int32_t delta_frames,
                                   int32_t type, int32_t byte_size,
                                   const uint8_t midi_data[4]) {
    if (!dst || !midi_data || type != 1 || byte_size < 1 || delta_frames < 0) {
        if (dst) dst->dropped++;
        return 0;
    }
    const uint8_t status = midi_data[0];
    uint32_t len;
    if (status >= 0x80 && status <= 0xEF)
        len = ((status & 0xE0u) == 0xC0u) ? 2u : 3u;
    else if (status == 0xF1 || status == 0xF3)
        len = 2u;
    else if (status == 0xF2)
        len = 3u;
    else if (status == 0xF6 || status >= 0xF8)
        len = 1u;
    else {
        dst->dropped++;
        return 0;
    }
    return vst_host_midi_append(dst, (uint32_t)delta_frames, midi_data, len);
}

int vst_host_midi_append_vst_sysex(VstHostMidiBuffer* dst, int32_t delta_frames,
                                   int32_t type, int32_t byte_size,
                                   int32_t dump_bytes, const uint8_t* dump) {
    (void)byte_size;
    if (!dst || !dump || type != 6 || delta_frames < 0 || dump_bytes < 2 ||
        (uint32_t)dump_bytes > VST_HOST_MIDI_MAX_PAYLOAD || dump[0] != 0xF0 ||
        dump[dump_bytes - 1] != 0xF7) {
        if (dst) dst->dropped++;
        return 0;
    }
    return vst_host_midi_append(dst, (uint32_t)delta_frames, dump,
                                (uint32_t)dump_bytes);
}
