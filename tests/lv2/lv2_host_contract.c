#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char* const kPluginUri =
    "https://guitarrackcraft.test/lv2/host-contract";
static const uint32_t kOutputCapacity = 524288u;
static const int32_t kTimeMarker = 0x4c563254;

typedef struct {
    LV2_URID atomSequence;
    LV2_URID atomObject;
    LV2_URID atomInt;
    LV2_URID atomLong;
    LV2_URID atomFloat;
    LV2_URID midiEvent;
    LV2_URID timePosition;
    LV2_URID timeFrame;
    LV2_URID timeSpeed;
    LV2_URID timeBeatsPerMinute;
    LV2_URID timeBeatsPerBar;
    LV2_URID timeBeatUnit;
    LV2_URID timeBar;
    LV2_URID timeBarBeat;
    LV2_URID_Map* map;
    LV2_Atom_Sequence* input;
    LV2_Atom_Sequence* output;
} HostContract;

static LV2_URID map_uri(HostContract* self, const char* uri) {
    return self->map->map(self->map->handle, uri);
}

static LV2_Handle instantiate(const LV2_Descriptor* descriptor,
                              double sample_rate,
                              const char* bundle_path,
                              const LV2_Feature* const* features) {
    (void)descriptor;
    (void)sample_rate;
    (void)bundle_path;

    HostContract* self = (HostContract*)calloc(1, sizeof(HostContract));
    if (!self) return NULL;
    for (const LV2_Feature* const* feature = features; feature && *feature; ++feature) {
        if (strcmp((*feature)->URI, LV2_URID__map) == 0)
            self->map = (LV2_URID_Map*)(*feature)->data;
    }
    if (!self->map) {
        free(self);
        return NULL;
    }

    self->atomSequence = map_uri(self, LV2_ATOM__Sequence);
    self->atomObject = map_uri(self, LV2_ATOM__Object);
    self->atomInt = map_uri(self, LV2_ATOM__Int);
    self->atomLong = map_uri(self, LV2_ATOM__Long);
    self->atomFloat = map_uri(self, LV2_ATOM__Float);
    self->midiEvent = map_uri(self, LV2_MIDI__MidiEvent);
    self->timePosition = map_uri(self, LV2_TIME__Position);
    self->timeFrame = map_uri(self, LV2_TIME__frame);
    self->timeSpeed = map_uri(self, LV2_TIME__speed);
    self->timeBeatsPerMinute = map_uri(self, LV2_TIME__beatsPerMinute);
    self->timeBeatsPerBar = map_uri(self, LV2_TIME__beatsPerBar);
    self->timeBeatUnit = map_uri(self, LV2_TIME__beatUnit);
    self->timeBar = map_uri(self, LV2_TIME__bar);
    self->timeBarBeat = map_uri(self, LV2_TIME__barBeat);
    return (LV2_Handle)self;
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data) {
    HostContract* self = (HostContract*)instance;
    if (port == 0) self->input = (LV2_Atom_Sequence*)data;
    else if (port == 1) self->output = (LV2_Atom_Sequence*)data;
}

static int append_event(LV2_Atom_Sequence* sequence,
                        const LV2_Atom* body,
                        int64_t frame) {
    const uint32_t eventBytes = (uint32_t)sizeof(LV2_Atom_Event) + body->size;
    const uint32_t padded = (eventBytes + 7u) & ~7u;
    if (sequence->atom.size < sizeof(LV2_Atom_Sequence_Body) ||
        padded > kOutputCapacity - sizeof(LV2_Atom) - sequence->atom.size)
        return 0;

    LV2_Atom_Event* event = (LV2_Atom_Event*)
        ((uint8_t*)&sequence->body + sequence->atom.size);
    event->time.frames = frame;
    event->body = *body;
    memcpy((uint8_t*)event + sizeof(LV2_Atom_Event),
           (const uint8_t*)body + sizeof(LV2_Atom), body->size);
    if (padded > eventBytes)
        memset((uint8_t*)event + eventBytes, 0, padded - eventBytes);
    sequence->atom.size += padded;
    return 1;
}

static int valid_time_position(const HostContract* self,
                               const LV2_Atom_Object* object) {
    const LV2_Atom* frame = NULL;
    const LV2_Atom* speed = NULL;
    const LV2_Atom* bpm = NULL;
    const LV2_Atom* beats_per_bar = NULL;
    const LV2_Atom* beat_unit = NULL;
    const LV2_Atom* bar = NULL;
    const LV2_Atom* bar_beat = NULL;
    LV2_Atom_Object_Query query[] = {
        {self->timeFrame, &frame},
        {self->timeSpeed, &speed},
        {self->timeBeatsPerMinute, &bpm},
        {self->timeBeatsPerBar, &beats_per_bar},
        {self->timeBeatUnit, &beat_unit},
        {self->timeBar, &bar},
        {self->timeBarBeat, &bar_beat},
        LV2_ATOM_OBJECT_QUERY_END
    };
    if (object->body.otype != self->timePosition ||
        lv2_atom_object_query(object, query) != 7)
        return 0;
    if (!frame || frame->type != self->atomLong || frame->size != sizeof(int64_t) ||
        *(const int64_t*)((const uint8_t*)frame + sizeof(LV2_Atom)) != 123456789LL)
        return 0;
    if (!speed || speed->type != self->atomFloat || speed->size != sizeof(float) ||
        *(const float*)((const uint8_t*)speed + sizeof(LV2_Atom)) != 1.0f)
        return 0;
    if (!bpm || bpm->type != self->atomFloat || bpm->size != sizeof(float) ||
        *(const float*)((const uint8_t*)bpm + sizeof(LV2_Atom)) != 137.5f)
        return 0;
    if (!beats_per_bar || beats_per_bar->type != self->atomFloat ||
        beats_per_bar->size != sizeof(float) ||
        *(const float*)((const uint8_t*)beats_per_bar + sizeof(LV2_Atom)) != 7.0f)
        return 0;
    if (!beat_unit || beat_unit->type != self->atomInt || beat_unit->size != sizeof(int32_t) ||
        *(const int32_t*)((const uint8_t*)beat_unit + sizeof(LV2_Atom)) != 8)
        return 0;
    if (!bar || bar->type != self->atomLong || bar->size != sizeof(int64_t) ||
        *(const int64_t*)((const uint8_t*)bar + sizeof(LV2_Atom)) != 42LL)
        return 0;
    if (!bar_beat || bar_beat->type != self->atomFloat || bar_beat->size != sizeof(float) ||
        *(const float*)((const uint8_t*)bar_beat + sizeof(LV2_Atom)) != 2.5f)
        return 0;
    return 1;
}

static void run(LV2_Handle instance, uint32_t sample_count) {
    (void)sample_count;
    HostContract* self = (HostContract*)instance;
    if (!self->input || !self->output) return;

    self->output->atom.type = self->atomSequence;
    self->output->atom.size = sizeof(LV2_Atom_Sequence_Body);
    self->output->body.unit = 0;
    self->output->body.pad = 0;

    if (self->input->atom.type != self->atomSequence ||
        self->input->atom.size < sizeof(LV2_Atom_Sequence_Body))
        return;

    int saw_valid_time = 0;
    LV2_ATOM_SEQUENCE_FOREACH(self->input, event) {
        if (event->body.type == self->atomObject &&
            event->body.size >= sizeof(LV2_Atom_Object_Body)) {
            saw_valid_time |= valid_time_position(self,
                (const LV2_Atom_Object*)&event->body);
        } else {
            append_event(self->output, &event->body, event->time.frames);
        }
    }

    if (saw_valid_time) {
        struct {
            LV2_Atom atom;
            int32_t value;
        } marker = {{sizeof(int32_t), self->atomInt}, kTimeMarker};
        append_event(self->output, &marker.atom, 0);
    }
}

static void cleanup(LV2_Handle instance) {
    free(instance);
}

static const LV2_Descriptor kDescriptor = {
    kPluginUri,
    instantiate,
    connect_port,
    NULL,
    run,
    NULL,
    cleanup,
    NULL,
};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return index == 0 ? &kDescriptor : NULL;
}
