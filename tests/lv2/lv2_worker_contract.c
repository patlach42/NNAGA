#include <lv2/atom/atom.h>
#include <lv2/core/lv2.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char* const kPluginUri =
    "https://guitarrackcraft.test/lv2/worker-contract";
static const uint32_t kAtomSequenceCapacity = 8192u;

typedef struct {
    LV2_URID atomSequence;
    LV2_URID atomInt;
    LV2_URID_Map* map;
    LV2_Worker_Schedule* schedule;
    LV2_Atom_Sequence* output;
    float* mode;
    float* modeStorage;
    _Atomic uint32_t responseDrops;
    _Atomic int gate;
} WorkerContract;

static LV2_URID map_uri(WorkerContract* self, const char* uri) {
    return self->map->map(self->map->handle, uri);
}

static int append_int(WorkerContract* self, int32_t value) {
    LV2_Atom_Sequence* sequence = self->output;
    if (!sequence || sequence->atom.size < sizeof(LV2_Atom_Sequence_Body)) return 0;
    const uint32_t eventBytes = (uint32_t)sizeof(LV2_Atom_Event) + sizeof(value);
    const uint32_t padded = (eventBytes + 7u) & ~7u;
    if (padded > kAtomSequenceCapacity - sizeof(LV2_Atom) - sequence->atom.size)
        return 0;
    LV2_Atom_Event* event = (LV2_Atom_Event*)
        ((uint8_t*)&sequence->body + sequence->atom.size);
    event->time.frames = 0;
    event->body.type = self->atomInt;
    event->body.size = sizeof(value);
    memcpy((uint8_t*)&event->body + sizeof(LV2_Atom), &value, sizeof(value));
    if (padded > eventBytes)
        memset((uint8_t*)event + eventBytes, 0, padded - eventBytes);
    sequence->atom.size += padded;
    return 1;
}

static LV2_Handle instantiate(const LV2_Descriptor* descriptor,
                              double sample_rate,
                              const char* bundle_path,
                              const LV2_Feature* const* features) {
    (void)descriptor;
    (void)sample_rate;
    (void)bundle_path;
    WorkerContract* self = (WorkerContract*)calloc(1, sizeof(WorkerContract));
    if (!self) return NULL;
    for (const LV2_Feature* const* feature = features; feature && *feature; ++feature) {
        if (strcmp((*feature)->URI, LV2_URID__map) == 0)
            self->map = (LV2_URID_Map*)(*feature)->data;
        else if (strcmp((*feature)->URI, LV2_WORKER__schedule) == 0)
            self->schedule = (LV2_Worker_Schedule*)(*feature)->data;
    }
    if (!self->map || !self->schedule) {
        free(self);
        return NULL;
    }
    self->atomSequence = map_uri(self, LV2_ATOM__Sequence);
    self->atomInt = map_uri(self, LV2_ATOM__Int);
    self->modeStorage = (float*)calloc(1, sizeof(float));
    self->mode = self->modeStorage;
    atomic_init(&self->responseDrops, 0);
    atomic_init(&self->gate, 1);
    if (!self->modeStorage) {
        free(self);
        return NULL;
    }
    return (LV2_Handle)self;
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data) {
    WorkerContract* self = (WorkerContract*)instance;
    if (port == 0) self->mode = (float*)data;
    else if (port == 1) self->output = (LV2_Atom_Sequence*)data;
}

static void run(LV2_Handle instance, uint32_t sample_count) {
    (void)sample_count;
    WorkerContract* self = (WorkerContract*)instance;
    if (!self->output || !self->schedule || !self->mode) return;
    self->output->atom.type = self->atomSequence;
    self->output->atom.size = sizeof(LV2_Atom_Sequence_Body);
    self->output->body.unit = 0;
    self->output->body.pad = 0;

    const int mode = (int)*self->mode;
    append_int(self, 100 + mode);
    if (mode == 1) {
        const uint8_t request = 1;
        append_int(self, self->schedule->schedule_work(
            self->schedule->handle, sizeof(request), &request) == LV2_WORKER_SUCCESS ? 110 : 111);
    } else if (mode == 2) {
        const uint8_t request = 2;
        uint32_t accepted = 0;
        uint32_t rejected = 0;
        for (uint32_t i = 0; i < 128; ++i) {
            if (self->schedule->schedule_work(self->schedule->handle,
                                               sizeof(request), &request) == LV2_WORKER_SUCCESS)
                ++accepted;
            else
                ++rejected;
        }
        append_int(self, (int32_t)(3000 + accepted));
        append_int(self, (int32_t)(3100 + rejected));
    } else if (mode == 3) {
        const uint8_t request = 3;
        atomic_store_explicit(&self->gate, 0, memory_order_release);
        uint32_t accepted = 0;
        uint32_t rejected = 0;
        for (uint32_t i = 0; i < 128; ++i) {
            if (self->schedule->schedule_work(self->schedule->handle,
                                               sizeof(request), &request) == LV2_WORKER_SUCCESS)
                ++accepted;
            else
                ++rejected;
        }
        atomic_store_explicit(&self->gate, 1, memory_order_release);
        append_int(self, (int32_t)(3000 + accepted));
        append_int(self, (int32_t)(3100 + rejected));
    } else if (mode == 4) {
        const uint8_t request = 4;
        append_int(self, self->schedule->schedule_work(
            self->schedule->handle, 1, &request) == LV2_WORKER_SUCCESS ? 140 : 141);
    }
}

static LV2_Worker_Status work(LV2_Handle instance,
                              LV2_Worker_Respond_Function respond,
                              LV2_Worker_Respond_Handle handle,
                              uint32_t size,
                              const void* data) {
    WorkerContract* self = (WorkerContract*)instance;
    if (size != 1 || !data) return LV2_WORKER_ERR_UNKNOWN;
    const uint8_t mode = *(const uint8_t*)data;
    if (mode == 3) {
        while (!atomic_load_explicit(&self->gate, memory_order_acquire)) {}
    }
    if (mode == 1) {
        const int32_t response = 201;
        return respond(handle, sizeof(response), &response);
    }
    if (mode == 2) {
        for (int32_t i = 0; i < 128; ++i) {
            const int32_t response = 2000 + i;
            if (respond(handle, sizeof(response), &response) != LV2_WORKER_SUCCESS)
                atomic_fetch_add_explicit(&self->responseDrops, 1, memory_order_relaxed);
        }
    } else if (mode == 4) {
        uint8_t body = 4;
        uint8_t oversized[8193];
        memset(oversized, body, sizeof(oversized));
        if (respond(handle, sizeof(oversized), oversized) != LV2_WORKER_SUCCESS)
            atomic_fetch_add_explicit(&self->responseDrops, 1, memory_order_relaxed);
    }
    return LV2_WORKER_SUCCESS;
}

static LV2_Worker_Status work_response(LV2_Handle instance,
                                       uint32_t size,
                                       const void* body) {
    WorkerContract* self = (WorkerContract*)instance;
    if (size == sizeof(int32_t) && body) {
        int32_t value;
        memcpy(&value, body, sizeof(value));
        append_int(self, value + 10000);
    }
    return LV2_WORKER_SUCCESS;
}

static LV2_Worker_Status end_run(LV2_Handle instance) {
    WorkerContract* self = (WorkerContract*)instance;
    const uint32_t drops =
        atomic_exchange_explicit(&self->responseDrops, 0, memory_order_acq_rel);
    if (drops != 0) append_int(self, (int32_t)(3200 + drops));
    append_int(self, 30000);
    return LV2_WORKER_SUCCESS;
}

static const LV2_Worker_Interface kWorkerInterface = {
    work,
    work_response,
    end_run,
};

static const void* extension_data(const char* uri) {
    return strcmp(uri, LV2_WORKER__interface) == 0 ? &kWorkerInterface : NULL;
}

static void cleanup(LV2_Handle instance) {
    WorkerContract* self = (WorkerContract*)instance;
    free(self->modeStorage);
    free(self);
}

static const LV2_Descriptor kDescriptor = {
    kPluginUri,
    instantiate,
    connect_port,
    NULL,
    run,
    NULL,
    cleanup,
    extension_data,
};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return index == 0 ? &kDescriptor : NULL;
}
