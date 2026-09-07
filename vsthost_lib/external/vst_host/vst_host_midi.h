#ifndef VST_HOST_MIDI_H
#define VST_HOST_MIDI_H
#include <stdint.h>
#include <stddef.h>

#define VST_HOST_MIDI_MAX_EVENTS 128u
#define VST_HOST_MIDI_MAX_PAYLOAD 65536u

typedef struct {
    uint32_t frame_offset;
    uint32_t payload_offset;
    uint32_t payload_size;
} VstHostMidiDesc;

typedef struct {
    VstHostMidiDesc events[VST_HOST_MIDI_MAX_EVENTS];
    uint8_t payload[VST_HOST_MIDI_MAX_PAYLOAD];
    uint32_t count;
    uint32_t payload_size;
    uint32_t dropped;
} VstHostMidiBuffer;

void vst_host_midi_clear(VstHostMidiBuffer* b);
int vst_host_midi_append(VstHostMidiBuffer* b, uint32_t frame_offset,
                         const uint8_t* payload, uint32_t payload_size);
int vst_host_midi_copy(VstHostMidiBuffer* dst, const VstHostMidiBuffer* src);

/* Classify one borrowed VST2 event and append a complete message. Returns 1
 * on acceptance, 0 for malformed/unsupported/overflow. */
int vst_host_midi_append_vst_event(VstHostMidiBuffer* dst, int32_t delta_frames,
                                   int32_t type, int32_t byte_size,
                                   const uint8_t midi_data[4]);
int vst_host_midi_append_vst_sysex(VstHostMidiBuffer* dst, int32_t delta_frames,
                                   int32_t type, int32_t byte_size,
                                   int32_t dump_bytes, const uint8_t* dump);
/* Select the plugin's output even when it is intentionally empty. */
static inline VstHostMidiBuffer* vst_host_midi_select(VstHostMidiBuffer* current,
                                                       VstHostMidiBuffer* next,
                                                       int authoritative) {
    return authoritative ? next : current;
}
#endif
