// Cross-process shared-memory layout for the Android native VST IPC channel
// and the x86/x64 Wine guest running through FEX-Emu.
//
// Included by BOTH:
//   - vsthost_lib/src/main/cpp/ipc/SharedRing.{h,cpp} (ARM64 C++)
//   - external/vst_host{,_vst3} guest hosts          (x86/x64 C/C++)
//
// Keep this file pure C / stdint so both sides see the same layout.
// All atomics are 64-bit on a 64-byte cache line; offsets are stable across
// the two ABIs because both x86_64 and aarch64 use 8-byte alignment for
// uint64_t and the same struct-padding rules under -fno-pic in the guest
// and standard C++ on the host.

#ifndef VSTPOC_SHARED_LAYOUT_H
#define VSTPOC_SHARED_LAYOUT_H

#include <stdint.h>

#define VSTPOC_AUDIO_RING_FRAMES  16384   /* power of 2; ~340 ms at 48 kHz stereo */
#define VSTPOC_PARAM_RING_MSGS     64     /* power of 2 */
#define VSTPOC_CHANNELS            2
#define VSTPOC_CACHELINE           64
#define VSTPOC_MAX_PARAMS          1024   /* generic editor cap; large instruments commonly exceed 128 */
#define VSTPOC_PARAM_NAME_LEN      64     /* per-name buffer including NUL */
#define VSTPOC_PARAM_UNIT_LEN      24     /* display unit, e.g. dB/Hz/ms */
#define VSTPOC_PARAM_DISPLAY_LEN   64     /* current plugin-formatted value */

#define VSTPOC_SHARED_LAYOUT_MAGIC   UINT64_C(0x565354504f435338) /* "VSTPOCS8" */
#define VSTPOC_SHARED_LAYOUT_VERSION 8u
#define VSTPOC_TRANSPORT_QUEUE_CAPACITY 1024u
#define VSTPOC_FEATURE_PLANAR_AUDIO (UINT64_C(1) << 0)
#define VSTPOC_FEATURE_WAKE_SOCKET  (UINT64_C(1) << 1)
#define VSTPOC_FEATURE_MIDI_EVENTS (UINT64_C(1) << 2)
#define VSTPOC_FEATURE_MIDI_OUTPUT (UINT64_C(1) << 3)
#define VSTPOC_MAX_BLOCK_FRAMES 2048u
#define VSTPOC_MAX_MIDI_EVENTS_PER_BLOCK 128u

typedef struct {
    uint32_t frame_offset;
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
    uint8_t reserved;
} VstpocMidiEvent;

/* Native file-picker channel sizes. Wine-side GetOpenFileNameA hook writes
 * the request, Android-side SAF listener writes the response. */
#define VSTPOC_PICKER_TITLE_LEN     128
#define VSTPOC_PICKER_FILTER_LEN    512   /* Win32 filter spec: "Wave\0*.wav\0..." double-NUL ended */
#define VSTPOC_PICKER_PATH_LEN     1024

/* Native VST state transfer channel. The command path points at a temporary
 * host-created file; guest writes a SAVE blob there or reads a LOAD blob from
 * it. File transport avoids putting large plugin-owned chunks into shm. */
#define VSTPOC_STATE_PATH_LEN      1024
#define VSTPOC_STATE_MESSAGE_LEN    256

#define VSTPOC_STATE_CMD_NONE         0
#define VSTPOC_STATE_CMD_SAVE         1
#define VSTPOC_STATE_CMD_LOAD         2

#define VSTPOC_STATE_STATUS_IDLE      0
#define VSTPOC_STATE_STATUS_OK        1
#define VSTPOC_STATE_STATUS_ERROR     2
#define VSTPOC_STATE_STATUS_UNSUPPORTED 3

typedef struct {
    int32_t index;
    float   value;
} VstpocParamMsg;

/* Generic-editor metadata. Values on the control channel remain normalized.
 * step_count follows VST3 semantics: 0 continuous, 1 toggle, >1 discrete. */
#define VSTPOC_PARAM_FLAG_HIDDEN    (UINT32_C(1) << 0)
#define VSTPOC_PARAM_FLAG_READ_ONLY (UINT32_C(1) << 1)

typedef struct {
    float    default_normalized;
    int32_t  step_count;
    uint32_t flags;
    char     unit[VSTPOC_PARAM_UNIT_LEN];
} VstpocParamMetadata;

typedef struct {
    uint64_t sample_position;
    uint64_t transport_frame;
    uint64_t loop_end_frame;
    double sample_rate;
    double beats_per_minute;
    uint32_t flags;
    uint32_t block_frames;
    uint32_t midi_event_count;
    VstpocMidiEvent midi_events[VSTPOC_MAX_MIDI_EVENTS_PER_BLOCK];
} VstpocTransportBlock;

/* Single mmap region. Atomics laid out one-per-cacheline so producer and
 * consumer never share a line (avoids false sharing during heavy churn). */
typedef struct {
    /* control */
    _Alignas(VSTPOC_CACHELINE) uint64_t stop_flag;          /* host→guest: set 1 to ask guest to exit */
    _Alignas(VSTPOC_CACHELINE) uint64_t guest_ready;        /* guest→host: set 1 after mmap+init */
    _Alignas(VSTPOC_CACHELINE) uint64_t guest_frames_produced; /* monotonic counter for diagnostics */
    _Alignas(VSTPOC_CACHELINE) uint64_t mic_active;         /* host→guest: 1 = consume audio_in instead of generating */

    /* audio out ring: guest is producer, host is consumer */
    _Alignas(VSTPOC_CACHELINE) uint64_t audio_head;
    _Alignas(VSTPOC_CACHELINE) uint64_t audio_tail;

    /* audio in ring: host is producer (mic callback), guest is consumer */
    _Alignas(VSTPOC_CACHELINE) uint64_t audio_in_head;
    _Alignas(VSTPOC_CACHELINE) uint64_t audio_in_tail;

    /* param ring: host is producer, guest is consumer */
    _Alignas(VSTPOC_CACHELINE) uint64_t param_head;
    _Alignas(VSTPOC_CACHELINE) uint64_t param_tail;

    /* payload arrays, cache-line aligned so they don't share with the above */
    _Alignas(VSTPOC_CACHELINE) float          audio[VSTPOC_CHANNELS][VSTPOC_AUDIO_RING_FRAMES];
    _Alignas(VSTPOC_CACHELINE) float          audio_in[VSTPOC_CHANNELS][VSTPOC_AUDIO_RING_FRAMES];
    _Alignas(VSTPOC_CACHELINE) VstpocParamMsg params[VSTPOC_PARAM_RING_MSGS];

    /* metadata: written once by the guest at startup, BEFORE setting
     * guest_ready. The host reads after seeing guest_ready=1. */
    int32_t param_count;
    char    param_names[VSTPOC_MAX_PARAMS][VSTPOC_PARAM_NAME_LEN];

    /* plugin load status. Written by guest BEFORE it exits (success or
     * failure path). The host polls this so it can surface load errors
     * to the UI instead of silently showing a blank editor.
     *   0 = pending (guest still trying)
     *   1 = ok (set together with guest_ready=1)
     *   2 = failed — see status_message for reason */
    int32_t load_status;
    char    status_message[256];

    /* Plugin editor preferred size, in plugin-native pixels. Written by
     * vst_host after effEditGetRect succeeds; 0/0 if no editor or not yet
     * known. The Android side reads this to size the SurfaceView so the
     * plugin's GUI maps 1:1 in aspect (no letterbox, touch coords clean). */
    int32_t editor_width;
    int32_t editor_height;

    /* --- Health / diagnostics (added 2026-05-28) ---------------------
     * All fields are appended at the END of the struct to preserve
     * binary layout for older guest builds that don't write them — those
     * builds simply leave the trailing bytes at whatever the host
     * zero-initialised them to. New host reads should be tolerant of
     * fields reading 0 (= "not reported"). Use the diagnostic_layout_v
     * sentinel BELOW to detect a guest that pre-dates these fields
     * (writes 0) vs. one that writes ok-but-still-zero values. */
    uint32_t diagnostic_layout_v;     /* guest writes 1 when it knows the
                                        * fields below exist; 0 = legacy
                                        * guest, fields below meaningless */

    /* DXVK init status:
     *   0 = not attempted (no D3D11 used by plugin)
     *   1 = ok (vkCreateDevice succeeded, no memory alloc failures observed)
     *   2 = memory_alloc_fail (DxvkMemoryAllocator returned null at least
     *       once after the device was created — D3D11 may be partially
     *       broken; see last_memory_alloc_failed_*)
     *   3 = create_device_fail (vkCreateDevice returned non-success;
     *       D3D11InternalCreateDevice will have logged the error)
     *   4 = other failure (catch-all for ext errors) */
    uint32_t dxvk_init_status;

    /* D3D11 device status (separate from DXVK init because DXVK can be
     * loaded without ever creating a D3D11 device — e.g. plugins that
     * use DXGI for output enumeration only):
     *   0 = not_created (D3D11CreateDevice never called)
     *   1 = ok
     *   2 = failed */
    uint32_t d3d11_device_status;

    /* Bitmask of rendering APIs the plugin actually loaded:
     *   bit 0 = D3D11 (loaded d3d11.dll)
     *   bit 1 = D3D9  (loaded d3d9.dll)
     *   bit 2 = OpenGL (loaded opengl32.dll)
     *   bit 3 = GDI (loaded gdi32.dll AND issued BitBlt — passive load
     *           of gdi32 by combase etc. doesn't count)
     *   bit 4 = "none observed yet" (legacy default)
     * Black-screen detector: a plugin with bit 4 set AND
     * wm_user_storm_per_second > 100 AND paint_request_count == 0 is
     * almost certainly stuck in a JUCE event loop without rendering. */
    uint32_t render_api_used;

    /* DXVK memory allocation failure — most-recent attempt that returned
     * null. Set by patch 0003 in dxvk_memory.cpp. */
    uint64_t last_memory_alloc_failed_size;     /* VkMemoryRequirements::size */
    uint32_t last_memory_alloc_failed_types;    /* memoryTypeBits */
    uint32_t last_memory_alloc_failed_count;    /* monotonic; 0 = never */

    /* Render activity counters. paint_request_count covers BOTH WM_PAINT
     * messages dispatched to the editor hwnd AND X11 PutImage requests
     * (the Android-side X11 server counts these and writes via host JNI).
     * Used by the black-screen detector. */
    uint64_t paint_request_count;
    uint64_t wm_paint_count;

    /* VEH catalog hit bitmask — bit N = pattern N in g_veh_patterns hit
     * at least once. Capped at 64 patterns; if we exceed that the bit
     * becomes a tombstone "patterns N..64 collapsed". */
    uint64_t veh_patterns_hit_bitmask;

    /* JUCE WM_USER+123 storm rate, rolling 1s window. Plugins like TH-U
     * legitimately use this for timer ticks (~30/s); >5000/s is a
     * runaway storm that almost always coincides with a stuck editor
     * thread. */
    uint32_t wm_user_storm_per_second;

    /* Free-text written by the guest when it detects an anomaly itself
     * (matches load_status[256]/status_message[256] pair pattern). Set
     * once; subsequent anomalies overwrite. */
    char diagnostic_summary[256];

    /* Current normalized VST parameter values, written by the Wine guest
     * after param metadata is published and refreshed as the native editor
     * changes plugin state. Android reads this snapshot when saving presets
     * so VST editor edits round-trip instead of only host-slider edits.
     * param_values_seq is a seqlock: odd means write in progress, even means
     * stable, and 0 means the guest is older or has not published values yet. */
    _Alignas(VSTPOC_CACHELINE) uint64_t param_values_seq;
    _Alignas(VSTPOC_CACHELINE) float    param_values[VSTPOC_MAX_PARAMS];

    /* VST state command channel. Android writes state_command/state_path/
     * state_size, then increments state_request_seq. The Wine guest handles
     * the request on its plugin-control thread, writes state_status/state_size
     * and optional state_message, then stores state_response_seq=request_seq. */
    _Alignas(VSTPOC_CACHELINE) uint32_t state_request_seq;
    _Alignas(VSTPOC_CACHELINE) uint32_t state_response_seq;
    uint32_t state_command;
    uint32_t state_status;
    uint64_t state_size;
    char     state_path[VSTPOC_STATE_PATH_LEN];
    char     state_message[VSTPOC_STATE_MESSAGE_LEN];
    /* Transport ABI metadata and one seqlock snapshot. Appended only:
     * fields above retain their offsets for older mappings. */
    uint64_t shared_layout_magic;
    uint32_t shared_layout_version;
    uint32_t shared_layout_size;
    uint64_t shared_feature_bits;
    _Alignas(VSTPOC_CACHELINE) uint64_t transport_seq;
    _Alignas(VSTPOC_CACHELINE) uint64_t transport_sample_position;
    _Alignas(VSTPOC_CACHELINE) uint64_t transport_frame;
    _Alignas(VSTPOC_CACHELINE) double   transport_sample_rate;
    _Alignas(VSTPOC_CACHELINE) double   transport_beats_per_minute;
    _Alignas(VSTPOC_CACHELINE) uint32_t transport_flags; /* bit0 playing, bit1 looping */
    /* v3 bounded transport queue; appended after all v2 fields. */
    _Alignas(VSTPOC_CACHELINE) uint64_t transport_queue_head;
    _Alignas(VSTPOC_CACHELINE) uint64_t transport_queue_tail;
    _Alignas(VSTPOC_CACHELINE) VstpocTransportBlock transport_queue[VSTPOC_TRANSPORT_QUEUE_CAPACITY];
    /* Guest-produced MIDI for the most recently processed audio block. */
    _Alignas(VSTPOC_CACHELINE) uint64_t midi_output_seq;
    uint32_t midi_output_count;
    VstpocMidiEvent midi_output_events[VSTPOC_MAX_MIDI_EVENTS_PER_BLOCK];
    _Alignas(VSTPOC_CACHELINE) uint64_t transport_queue_dropped;

    /* Generic editor extension (layout v7). Metadata is published before
     * metadata_seq is incremented. Desired values form lossless coalescing
     * mailboxes: the host writes a value then increments that parameter's
     * sequence; the guest applies the newest value once per observed seq. */
    _Alignas(VSTPOC_CACHELINE) uint64_t param_metadata_seq;
    _Alignas(VSTPOC_CACHELINE) VstpocParamMetadata param_metadata[VSTPOC_MAX_PARAMS];
    _Alignas(VSTPOC_CACHELINE) char param_display_values[VSTPOC_MAX_PARAMS][VSTPOC_PARAM_DISPLAY_LEN];
    _Alignas(VSTPOC_CACHELINE) uint64_t param_desired_seq[VSTPOC_MAX_PARAMS];
    _Alignas(VSTPOC_CACHELINE) float param_desired_values[VSTPOC_MAX_PARAMS];

    /* Latency extension (layout v8), append-only. The guest publishes the
     * plugin's own delay and the fixed shared-ring bridge quantum. A seqlock
     * lets the host read both values without locks or torn updates. */
    _Alignas(VSTPOC_CACHELINE) uint64_t latency_seq;
    uint32_t plugin_latency_frames;
    uint32_t bridge_quantum_frames;
    uint32_t latency_layout_v;
    uint32_t latency_reserved;
} VstpocShared;

/* Native file-picker channel — lives in its OWN mmap file
 * (vst_picker_pN.dat next to vst_shm_pN.dat) so wine's comdlg32 hook
 * doesn't need to know the layout of VstpocShared.
 *
 * Wine flow (when env VSTPOC_PICKER_PATH is set + comdlg32 patch is
 * active): GetOpenFileNameA/W writes request_title / request_filter /
 * request_initial_dir, then bumps `request_seq` from N to N+1. It then
 * polls `response_seq` until it equals request_seq, sleeping ~25 ms.
 *
 * Android flow: a coroutine on the Compose side polls request_seq vs
 * response_seq. When request_seq is ahead, it parses the Win32 filter
 * into MIME types, launches ACTION_OPEN_DOCUMENT, copies the picked URI
 * into wineprefix/drive_c/users/<u>/Documents/IRs/<name>, fills
 * response_path with the resulting Windows path (or leaves it empty +
 * sets response_cancelled=1), then bumps response_seq to match.
 *
 * Multi-plugin: each plugin process has its own picker file (p0, p1,
 * …); the Android listener multiplexes across them. */
typedef struct {
    _Alignas(VSTPOC_CACHELINE) volatile uint32_t request_seq;
    _Alignas(VSTPOC_CACHELINE) volatile uint32_t response_seq;
    int32_t response_cancelled;     /* 1 = user cancelled, response_path may be empty */
    int32_t reserved0;
    char    request_title       [VSTPOC_PICKER_TITLE_LEN];
    char    request_filter      [VSTPOC_PICKER_FILTER_LEN];
    char    request_initial_dir [VSTPOC_PICKER_PATH_LEN];
    char    response_path       [VSTPOC_PICKER_PATH_LEN];
} VstpocPickerChannel;

#define VSTPOC_PARAM_INDEX_GAIN 0
#define VSTPOC_PARAM_INDEX_FREQ 1

#endif /* VSTPOC_SHARED_LAYOUT_H */
