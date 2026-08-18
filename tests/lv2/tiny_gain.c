#include <lv2/core/lv2.h>

#include <stdlib.h>

static const char* const kPluginUri = "https://guitarrackcraft.test/lv2/tiny-gain";

typedef struct {
    const float* input;
    float* output;
} TinyGain;

static LV2_Handle instantiate(const LV2_Descriptor* descriptor,
                              double sample_rate,
                              const char* bundle_path,
                              const LV2_Feature* const* features) {
    (void)descriptor;
    (void)sample_rate;
    (void)bundle_path;
    (void)features;
    return calloc(1, sizeof(TinyGain));
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data) {
    TinyGain* self = (TinyGain*)instance;
    if (port == 0) {
        self->input = (const float*)data;
    } else if (port == 1) {
        self->output = (float*)data;
    }
}

static void run(LV2_Handle instance, uint32_t sample_count) {
    TinyGain* self = (TinyGain*)instance;
    for (uint32_t i = 0; i < sample_count; ++i) {
        self->output[i] = self->input[i] * 2.0f;
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
