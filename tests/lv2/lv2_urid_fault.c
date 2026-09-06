#include <lv2/core/lv2.h>
#include <lv2/urid/urid.h>

#include <stdlib.h>
#include <string.h>

static const char* const kPluginUri =
    "https://guitarrackcraft.test/lv2/urid-fault";
static const char* const kRuntimeOnlyUri =
    "https://guitarrackcraft.test/lv2/runtime-only-urid-v1";

typedef struct {
    const float* input;
    float* output;
    LV2_URID_Map* map;
} UridFault;

static LV2_Handle instantiate(const LV2_Descriptor* descriptor,
                              double sample_rate,
                              const char* bundle_path,
                              const LV2_Feature* const* features) {
    (void)descriptor;
    (void)sample_rate;
    (void)bundle_path;
    UridFault* self = (UridFault*)calloc(1, sizeof(UridFault));
    if (!self) return NULL;
    for (const LV2_Feature* const* feature = features; feature && *feature; ++feature) {
        if (strcmp((*feature)->URI, LV2_URID__map) == 0)
            self->map = (LV2_URID_Map*)(*feature)->data;
    }
    return (LV2_Handle)self;
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data) {
    UridFault* self = (UridFault*)instance;
    if (port == 0) self->input = (const float*)data;
    else if (port == 1) self->output = (float*)data;
}

static void run(LV2_Handle instance, uint32_t sample_count) {
    UridFault* self = (UridFault*)instance;
    if (self->map && self->map->map)
        (void)self->map->map(self->map->handle, kRuntimeOnlyUri);
    for (uint32_t i = 0; i < sample_count; ++i)
        self->output[i] = -123.0f;
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
