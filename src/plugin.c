/*
 * ******************************************************************
 * Zynthian Slicer LV2 plugin
 *
 * Splits an audio sample into slices played by consecutive MIDI notes.
 *
 * Threads:
 *  - The audio thread (run) plays voices and turns control changes into
 *    worker jobs. It never allocates, frees or touches files.
 *  - The worker thread (work) owns the slicing model: it loads samples,
 *    slices them, applies edits, reads and writes slice maps and exports SFZ.
 *    Each job answers with a heap result holding a new sample and/or slice
 *    table and the values to report back to the host. The audio thread swaps
 *    them in and sends the old buffers back to the worker to be freed.
 *
 * Files:
 *  - Live slice file /tmp/zynthian-slicer-<instance_id>.slices: the current
 *    slices for the Zynthian widget, rewritten after every change. The widget
 *    finds it through the instance_id output port and re-reads it when
 *    live_version changes.
 *  - Saved slice maps $ZYNTHIAN_MY_DATA_DIR/slicer/<hash>.slices: hand-edited
 *    slices, named by content. Their path is reported through the slice_map
 *    property, so Zynthian snapshots restore them.
 *
 * ******************************************************************
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * For a full copy of the GNU General Public License see the LICENSE.txt file.
 *
 * ******************************************************************
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/core/lv2_util.h>
#include <lv2/log/logger.h>
#include <lv2/midi/midi.h>
#include <lv2/patch/patch.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#include "common.h"
#include "dsp.h"
#include "export.h"
#include "model.h"
#include "sample.h"

// ----------------------------------------------------------------------------
// Ports (keep in sync with slicer.lv2/slicer.ttl)
// ----------------------------------------------------------------------------

enum {
    P_CONTROL = 0,      // Atom input: MIDI + patch messages
    P_NOTIFY,           // Atom output: patch messages
    P_OUT_L,
    P_OUT_R,
    P_GAIN,
    P_TUNE,
    P_BASE_NOTE,
    P_POLYPHONY,
    P_SLICE_MODE,
    P_SLICE_COUNT,
    P_SLICE_BEATS,
    P_SLICE_SUBDIVISION,
    P_THRESHOLD,
    P_MIN_GAP,
    P_ZOOM,
    P_AUDITION,
    P_INSTANCE_ID,      // Outputs (monitors)
    P_LIVE_VERSION,
    P_EXPORT_COUNT,
    P_PLAYHEAD,
    P_ACTIVE_0,         // Playing slices, 24 bits per port
    P_ACTIVE_LAST = P_ACTIVE_0 + 5,
    P_COUNT
};
#define ACTIVE_BITS 24  // Integers up to 2^24 are exact in a float

// ----------------------------------------------------------------------------
// Worker messages
// ----------------------------------------------------------------------------

enum JOB {
    JOB_LOAD_SAMPLE = 1,
    JOB_LOAD_MAP,
    JOB_PARAMS,
    JOB_DETECT,
    JOB_SELECT,
    JOB_SET_START,
    JOB_SET_END,
    JOB_SET_MODE,
    JOB_SAVE_MAP,
    JOB_EXPORT,
    JOB_FREE,
    JOB_TYPES
};

enum GARBAGE {
    GARBAGE_SAMPLE = 1,
    GARBAGE_TABLE,
    GARBAGE_RESULT
};

typedef struct {
    uint32_t type;
    union {
        SliceParams params;
        int32_t ival;
        float fval;
        struct {
            float gain_db;
            int32_t tune;
            int32_t polyphony;
        } export_options;
        struct {
            uint32_t kind;
            void* ptr;
        } garbage;
    } u;
    char path[SLICER_PATH_MAX]; // JOB_LOAD_SAMPLE / JOB_LOAD_MAP only (sent truncated)
} Job;
#define JOB_HEADER_SIZE (offsetof(Job, path))

// Feedback flags
#define FB_SELECTION 0x01   // slice, slice_start, slice_end, play_mode changed
#define FB_MAP 0x02         // slice_map path changed
#define FB_LIVE 0x04        // Live slice file rewritten
#define FB_EXPORT 0x08      // SFZ export finished
#define FB_SAVE 0x10        // Slices changed: save the slice map soon
#define FB_TRIGGERS 0x20    // Reset the detect and export_sfz triggers

typedef struct {
    Sample* sample;         // New sample, owned by the audio thread once received
    int clear_sample;       // 1 to unload the sample
    SliceTable* table;      // New slice table
    uint32_t flags;
    int32_t selected;
    float start_pct;
    float end_pct;
    int32_t play_mode;
    char map_path[SLICER_PATH_MAX];
} WorkResult;

typedef struct {
    LV2_URID atom_Path;
    LV2_URID atom_String;
    LV2_URID atom_Float;
    LV2_URID atom_Double;
    LV2_URID atom_Int;
    LV2_URID atom_Long;
    LV2_URID atom_Bool;
    LV2_URID atom_URID;
    LV2_URID atom_Object;
    LV2_URID atom_Blank;
    LV2_URID atom_Sequence;
    LV2_URID atom_eventTransfer;
    LV2_URID midi_Event;
    LV2_URID patch_Set;
    LV2_URID patch_Get;
    LV2_URID patch_property;
    LV2_URID patch_value;
    LV2_URID p_sample;
    LV2_URID p_slice_map;
    LV2_URID p_slice;
    LV2_URID p_slice_start;
    LV2_URID p_slice_end;
    LV2_URID p_play_mode;
    LV2_URID p_detect;
    LV2_URID p_export_sfz;
} Urids;

#define MAX_GARBAGE 64
#define SAVE_DELAY_SECONDS 1.0  // Wait for edits to settle before saving the slice map
// Slice edits arriving this soon after a sample or slice map are ignored:
// they are a Zynthian snapshot being restored (its hand edits come from the
// slice map), sent in no guaranteed order, so they could hit the wrong slice.
#define RESTORE_GUARD_SECONDS 0.5

typedef struct {
    uint32_t kind;
    void* ptr;
} Garbage;

typedef struct {
    // Host features
    LV2_URID_Map* map;
    LV2_Worker_Schedule* schedule;
    LV2_Log_Logger logger;
    Urids u;
    LV2_Atom_Forge forge;
    double rate;

    // Ports
    const LV2_Atom_Sequence* control;
    LV2_Atom_Sequence* notify;
    float* out_l;
    float* out_r;
    const float* in[P_COUNT];
    float* mon[P_COUNT];

    // ---- Audio thread ----
    Sample* sample;
    SliceTable* table;
    VoicePool pool;

    SliceParams sent_params;
    int params_sent;
    int audition;           // Auditioned slice + 1, or 0
    uint8_t audition_note;

    uint32_t live_version;
    uint32_t export_count;
    int64_t save_countdown; // Frames until the slice map is saved, or -1
    uint64_t frame_clock;   // Frames processed since activation
    uint64_t restore_guard; // Ignore slice edits until frame_clock reaches this

    // Jobs that couldn't be scheduled yet (latest wins)
    int pending[JOB_TYPES];
    Job pending_job[JOB_TYPES];

    Garbage garbage[MAX_GARBAGE];
    int garbage_count;

    // Values to report to the host on the next cycle
    uint32_t fb_flags;
    int32_t fb_selected;
    float fb_start_pct;
    float fb_end_pct;
    int32_t fb_play_mode;
    char fb_map_path[SLICER_PATH_MAX];

    // ---- Worker thread ----
    Model* model;
    uint32_t instance_id;
    char live_path[SLICER_PATH_MAX];
    char maps_dir[SLICER_PATH_MAX];
    char sfz_dir[SLICER_PATH_MAX];
    char map_path[SLICER_PATH_MAX]; // Slice map file matching the model, or ""
    char* text;                     // Scratch buffer for slice map text
    size_t text_size;
} Slicer;

// ----------------------------------------------------------------------------
// Audio thread helpers
// ----------------------------------------------------------------------------

static size_t job_size(const Job* job) {
    if (job->type == JOB_LOAD_SAMPLE || job->type == JOB_LOAD_MAP)
        return JOB_HEADER_SIZE + strlen(job->path) + 1;
    return JOB_HEADER_SIZE;
}

/* Schedule a job, or keep it pending (replacing an older one of its type). */
static void request(Slicer* self, const Job* job) {
    if (job->type != JOB_FREE && !self->pending[job->type]
            && self->schedule->schedule_work(self->schedule->handle, job_size(job), job) == LV2_WORKER_SUCCESS)
        return;
    if (job->type == JOB_FREE)
        return; // Garbage has its own queue
    // Keep order simple: a pending job of this type is replaced by the latest
    memcpy(&self->pending_job[job->type], job, job_size(job));
    self->pending[job->type] = 1;
}

static void request_simple(Slicer* self, uint32_t type) {
    Job job;
    job.type = type;
    request(self, &job);
}

static void flush_pending(Slicer* self) {
    for (uint32_t t = 1; t < JOB_TYPES; t++) {
        if (!self->pending[t])
            continue;
        Job* job = &self->pending_job[t];
        if (self->schedule->schedule_work(self->schedule->handle, job_size(job), job) != LV2_WORKER_SUCCESS)
            return; // Keep the order: try again next cycle
        self->pending[t] = 0;
    }
}

static void add_garbage(Slicer* self, uint32_t kind, void* ptr) {
    if (!ptr)
        return;
    if (self->garbage_count < MAX_GARBAGE) {
        self->garbage[self->garbage_count].kind = kind;
        self->garbage[self->garbage_count].ptr = ptr;
        self->garbage_count++;
    }
    // A full queue leaks rather than freeing on the audio thread
}

static void flush_garbage(Slicer* self) {
    while (self->garbage_count > 0) {
        Garbage* g = &self->garbage[self->garbage_count - 1];
        Job job;
        job.type = JOB_FREE;
        job.u.garbage.kind = g->kind;
        job.u.garbage.ptr = g->ptr;
        if (self->schedule->schedule_work(self->schedule->handle, JOB_HEADER_SIZE, &job) != LV2_WORKER_SUCCESS)
            return;
        self->garbage_count--;
    }
}

static void request_path(Slicer* self, uint32_t type, const char* path, uint32_t len) {
    Job job;
    job.type = type;
    if (len >= sizeof(job.path))
        len = sizeof(job.path) - 1;
    memcpy(job.path, path, len);
    job.path[len] = 0;
    // Zynthian sends "_" for an empty path
    if (!strcmp(job.path, "_"))
        job.path[0] = 0;
    request(self, &job);
}

static int atom_number(const Slicer* self, const LV2_Atom* atom, double* value) {
    const Urids* u = &self->u;
    if (atom->type == u->atom_Float)
        *value = ((const LV2_Atom_Float*)atom)->body;
    else if (atom->type == u->atom_Double)
        *value = ((const LV2_Atom_Double*)atom)->body;
    else if (atom->type == u->atom_Int)
        *value = ((const LV2_Atom_Int*)atom)->body;
    else if (atom->type == u->atom_Long)
        *value = (double)((const LV2_Atom_Long*)atom)->body;
    else if (atom->type == u->atom_Bool)
        *value = ((const LV2_Atom_Bool*)atom)->body;
    else
        return 0;
    return 1;
}

static void handle_patch_set(Slicer* self, const LV2_Atom_Object* obj) {
    const Urids* u = &self->u;
    const LV2_Atom* property = NULL;
    const LV2_Atom* value = NULL;
    lv2_atom_object_get(obj, u->patch_property, &property, u->patch_value, &value, 0);
    if (!property || property->type != u->atom_URID || !value)
        return;
    LV2_URID key = ((const LV2_Atom_URID*)property)->body;

    if (key == u->p_sample || key == u->p_slice_map) {
        if (value->type != u->atom_Path && value->type != u->atom_String)
            return;
        const char* path = (const char*)LV2_ATOM_BODY_CONST(value);
        uint32_t len = (uint32_t)strnlen(path, value->size);
        request_path(self, key == u->p_sample ? JOB_LOAD_SAMPLE : JOB_LOAD_MAP, path, len);
        self->restore_guard = self->frame_clock + (uint64_t)(self->rate * RESTORE_GUARD_SECONDS);
        return;
    }
    if ((key == u->p_slice_start || key == u->p_slice_end || key == u->p_play_mode)
            && self->frame_clock < self->restore_guard) {
        // Report the real values back over the restored ones
        self->fb_flags |= FB_SELECTION;
        return;
    }

    double number;
    if (!atom_number(self, value, &number))
        return;
    Job job;
    if (key == u->p_detect || key == u->p_export_sfz) {
        // Triggers: Zynthian never sends them back to 0, so every message
        // with a value counts as a press, and the plugin resets them.
        self->fb_flags |= FB_TRIGGERS;
        if (number < 0.5)
            return;
        if (key == u->p_detect) {
            job.type = JOB_DETECT;
        } else {
            job.type = JOB_EXPORT;
            job.u.export_options.gain_db = *self->in[P_GAIN];
            job.u.export_options.tune = (int32_t)lrintf(*self->in[P_TUNE]);
            job.u.export_options.polyphony = (int32_t)lrintf(*self->in[P_POLYPHONY]);
        }
    } else if (key == u->p_slice) {
        job.type = JOB_SELECT;
        job.u.ival = (int32_t)lrint(number);
    } else if (key == u->p_slice_start) {
        job.type = JOB_SET_START;
        job.u.fval = (float)number;
    } else if (key == u->p_slice_end) {
        job.type = JOB_SET_END;
        job.u.fval = (float)number;
    } else if (key == u->p_play_mode) {
        job.type = JOB_SET_MODE;
        job.u.ival = (int32_t)lrint(number);
    } else {
        return;
    }
    request(self, &job);
}

static void forge_set_number(Slicer* self, LV2_URID key, int is_int, double value) {
    LV2_Atom_Forge* forge = &self->forge;
    LV2_Atom_Forge_Frame frame;
    if (!lv2_atom_forge_frame_time(forge, 0))
        return;
    if (!lv2_atom_forge_object(forge, &frame, 0, self->u.patch_Set))
        return;
    lv2_atom_forge_key(forge, self->u.patch_property);
    lv2_atom_forge_urid(forge, key);
    lv2_atom_forge_key(forge, self->u.patch_value);
    if (is_int)
        lv2_atom_forge_int(forge, (int32_t)value);
    else
        lv2_atom_forge_float(forge, (float)value);
    lv2_atom_forge_pop(forge, &frame);
}

static void forge_set_path(Slicer* self, LV2_URID key, const char* path) {
    LV2_Atom_Forge* forge = &self->forge;
    LV2_Atom_Forge_Frame frame;
    if (!lv2_atom_forge_frame_time(forge, 0))
        return;
    if (!lv2_atom_forge_object(forge, &frame, 0, self->u.patch_Set))
        return;
    lv2_atom_forge_key(forge, self->u.patch_property);
    lv2_atom_forge_urid(forge, key);
    lv2_atom_forge_key(forge, self->u.patch_value);
    lv2_atom_forge_path(forge, path, (uint32_t)strlen(path));
    lv2_atom_forge_pop(forge, &frame);
}

static void send_feedback(Slicer* self) {
    if (self->fb_flags & FB_SELECTION) {
        forge_set_number(self, self->u.p_slice, 1, self->fb_selected);
        forge_set_number(self, self->u.p_slice_start, 0, self->fb_start_pct);
        forge_set_number(self, self->u.p_slice_end, 0, self->fb_end_pct);
        forge_set_number(self, self->u.p_play_mode, 1, self->fb_play_mode);
    }
    if (self->fb_flags & FB_MAP)
        forge_set_path(self, self->u.p_slice_map, self->fb_map_path);
    if (self->fb_flags & FB_TRIGGERS) {
        forge_set_number(self, self->u.p_detect, 1, 0);
        forge_set_number(self, self->u.p_export_sfz, 1, 0);
    }
    self->fb_flags &= ~(FB_SELECTION | FB_MAP | FB_TRIGGERS);
}

static void read_params(const Slicer* self, SliceParams* p) {
    p->slice_mode = (int)lrintf(*self->in[P_SLICE_MODE]);
    p->slice_count = (int)lrintf(*self->in[P_SLICE_COUNT]);
    p->beats = (int)lrintf(*self->in[P_SLICE_BEATS]);
    p->subdivision = (int)lrintf(*self->in[P_SLICE_SUBDIVISION]);
    p->threshold = *self->in[P_THRESHOLD];
    p->min_gap_ms = *self->in[P_MIN_GAP];
    p->base_note = (int)lrintf(*self->in[P_BASE_NOTE]);
}

static void update_audition(Slicer* self) {
    int value = (int)lrintf(*self->in[P_AUDITION]);
    if (value < 0 || value > MAX_SLICES)
        value = 0;
    if (value == self->audition)
        return;
    if (self->audition)
        dsp_note_off(&self->pool, self->table, self->audition_note);
    self->audition = value;
    if (value && self->table) {
        int note = self->table->base_note + value - 1;
        if (note >= 1 && note <= MAX_NOTE) {
            self->audition_note = (uint8_t)note;
            dsp_note_on(&self->pool, self->table, self->audition_note, 127, 0);
            return;
        }
    }
    self->audition = 0;
}

// ----------------------------------------------------------------------------
// LV2 plugin interface
// ----------------------------------------------------------------------------

static void map_urids(LV2_URID_Map* map, Urids* u) {
    u->atom_Path = map->map(map->handle, LV2_ATOM__Path);
    u->atom_String = map->map(map->handle, LV2_ATOM__String);
    u->atom_Float = map->map(map->handle, LV2_ATOM__Float);
    u->atom_Double = map->map(map->handle, LV2_ATOM__Double);
    u->atom_Int = map->map(map->handle, LV2_ATOM__Int);
    u->atom_Long = map->map(map->handle, LV2_ATOM__Long);
    u->atom_Bool = map->map(map->handle, LV2_ATOM__Bool);
    u->atom_URID = map->map(map->handle, LV2_ATOM__URID);
    u->atom_Object = map->map(map->handle, LV2_ATOM__Object);
    u->atom_Blank = map->map(map->handle, LV2_ATOM__Blank);
    u->atom_Sequence = map->map(map->handle, LV2_ATOM__Sequence);
    u->atom_eventTransfer = map->map(map->handle, LV2_ATOM__eventTransfer);
    u->midi_Event = map->map(map->handle, LV2_MIDI__MidiEvent);
    u->patch_Set = map->map(map->handle, LV2_PATCH__Set);
    u->patch_Get = map->map(map->handle, LV2_PATCH__Get);
    u->patch_property = map->map(map->handle, LV2_PATCH__property);
    u->patch_value = map->map(map->handle, LV2_PATCH__value);
    u->p_sample = map->map(map->handle, SLICER_URI "#sample");
    u->p_slice_map = map->map(map->handle, SLICER_URI "#slice_map");
    u->p_slice = map->map(map->handle, SLICER_URI "#slice");
    u->p_slice_start = map->map(map->handle, SLICER_URI "#slice_start");
    u->p_slice_end = map->map(map->handle, SLICER_URI "#slice_end");
    u->p_play_mode = map->map(map->handle, SLICER_URI "#play_mode");
    u->p_detect = map->map(map->handle, SLICER_URI "#detect");
    u->p_export_sfz = map->map(map->handle, SLICER_URI "#export_sfz");
}

static uint32_t make_instance_id(const void* self) {
    uint64_t seed = (uint64_t)time(NULL) ^ ((uint64_t)(uintptr_t)self << 7) ^ ((uint64_t)getpid() << 29)
                    ^ (uint64_t)clock();
    char buf[sizeof(seed)];
    memcpy(buf, &seed, sizeof(seed));
    uint32_t id = (uint32_t)(slicer_hash(buf, sizeof(buf)) & 0xffffff);
    return id ? id : 1;
}

static const char* live_dir(void) {
    const char* dir = getenv("ZYNTHIAN_SLICER_LIVE_DIR"); // For tests
    return dir && dir[0] ? dir : "/tmp";
}

static LV2_Handle instantiate(const LV2_Descriptor* descriptor, double rate,
                              const char* bundle_path, const LV2_Feature* const* features) {
    (void)descriptor;
    (void)bundle_path;
    Slicer* self = calloc(1, sizeof(Slicer));
    if (!self)
        return NULL;
    LV2_Log_Log* log = NULL;
    const char* missing = lv2_features_query(features,
        LV2_LOG__log, &log, false,
        LV2_URID__map, &self->map, true,
        LV2_WORKER__schedule, &self->schedule, true,
        NULL);
    lv2_log_logger_init(&self->logger, self->map, log);
    if (missing) {
        lv2_log_error(&self->logger, "Slicer: missing feature <%s>\n", missing);
        free(self);
        return NULL;
    }
    map_urids(self->map, &self->u);
    lv2_atom_forge_init(&self->forge, self->map);
    self->rate = rate;
    self->pool.polyphony = MAX_VOICES;
    self->save_countdown = -1;

    self->model = malloc(sizeof(Model));
    self->text_size = 4096 + MAX_SLICES * 40 + 2 * SLICER_PATH_MAX;
    self->text = malloc(self->text_size);
    if (!self->model || !self->text) {
        free(self->model);
        free(self->text);
        free(self);
        return NULL;
    }
    model_init(self->model);

    const char* data_dir = getenv("ZYNTHIAN_MY_DATA_DIR");
    if (!data_dir || !data_dir[0])
        data_dir = "/zynthian/zynthian-my-data";
    snprintf(self->maps_dir, sizeof(self->maps_dir), "%s/slicer", data_dir);
    snprintf(self->sfz_dir, sizeof(self->sfz_dir), "%s/soundfonts/sfz", data_dir);

    // A unique id, so each instance gets its own live slice file
    for (int tries = 0; tries < 100; tries++) {
        self->instance_id = make_instance_id(self) ^ (uint32_t)tries;
        self->instance_id = (self->instance_id & 0xffffff) ? self->instance_id & 0xffffff : 1;
        snprintf(self->live_path, sizeof(self->live_path), "%s/zynthian-slicer-%u.slices",
                 live_dir(), self->instance_id);
        if (access(self->live_path, F_OK) != 0)
            break;
    }
    return self;
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data) {
    Slicer* self = instance;
    switch (port) {
        case P_CONTROL:
            self->control = data;
            break;
        case P_NOTIFY:
            self->notify = data;
            break;
        case P_OUT_L:
            self->out_l = data;
            break;
        case P_OUT_R:
            self->out_r = data;
            break;
        default:
            if (port < P_COUNT) {
                if (port >= P_INSTANCE_ID)
                    self->mon[port] = data;
                else
                    self->in[port] = data;
            }
            break;
    }
}

static void activate(LV2_Handle instance) {
    Slicer* self = instance;
    dsp_reset(&self->pool);
    self->audition = 0;
}

static void run(LV2_Handle instance, uint32_t frames) {
    Slicer* self = instance;
    const Urids* u = &self->u;

    // Notify output
    const uint32_t capacity = self->notify->atom.size;
    lv2_atom_forge_set_buffer(&self->forge, (uint8_t*)self->notify, capacity);
    LV2_Atom_Forge_Frame notify_frame;
    lv2_atom_forge_sequence_head(&self->forge, &notify_frame, 0);

    flush_garbage(self);
    flush_pending(self);
    send_feedback(self);

    // Control events
    LV2_ATOM_SEQUENCE_FOREACH(self->control, ev) {
        if (ev->body.type == u->midi_Event) {
            const uint8_t* msg = (const uint8_t*)(ev + 1);
            if (ev->body.size < 3)
                continue;
            uint8_t status = msg[0] & 0xf0;
            uint8_t note = msg[1];
            uint8_t velocity = msg[2];
            uint32_t offset = ev->time.frames < frames ? (uint32_t)ev->time.frames : 0;
            if (status == LV2_MIDI_MSG_NOTE_ON && velocity) {
                if (note == 0)
                    dsp_note_off(&self->pool, self->table, 0); // Note 0 stops all voices
                else
                    dsp_note_on(&self->pool, self->table, note, velocity, offset);
            } else if (status == LV2_MIDI_MSG_NOTE_OFF || status == LV2_MIDI_MSG_NOTE_ON) {
                dsp_note_off(&self->pool, self->table, note);
            } else if (status == LV2_MIDI_MSG_CONTROLLER && (note == LV2_MIDI_CTL_ALL_NOTES_OFF
                                                            || note == LV2_MIDI_CTL_ALL_SOUNDS_OFF)) {
                dsp_note_off(&self->pool, self->table, 0);
            }
        } else if (lv2_atom_forge_is_object_type(&self->forge, ev->body.type)) {
            const LV2_Atom_Object* obj = (const LV2_Atom_Object*)&ev->body;
            if (obj->body.otype == u->patch_Set) {
                handle_patch_set(self, obj);
            } else if (obj->body.otype == u->patch_Get) {
                // Report the current values on the next cycle
                self->fb_flags |= FB_SELECTION | FB_MAP;
            }
        }
    }

    // Control ports
    dsp_set_polyphony(&self->pool, (int)lrintf(*self->in[P_POLYPHONY]));
    SliceParams params;
    read_params(self, &params);
    if (!self->params_sent || memcmp(&params, &self->sent_params, sizeof(params))) {
        Job job;
        job.type = JOB_PARAMS;
        job.u.params = params;
        request(self, &job);
        self->sent_params = params;
        self->params_sent = 1;
    }
    update_audition(self);

    // Audio
    memset(self->out_l, 0, frames * sizeof(float));
    memset(self->out_r, 0, frames * sizeof(float));
    float gain = powf(10.0f, *self->in[P_GAIN] / 20.0f);
    float tune = *self->in[P_TUNE];
    if (tune < -MAX_TUNE)
        tune = -MAX_TUNE;
    if (tune > MAX_TUNE)
        tune = MAX_TUNE;
    double rate = fabsf(tune) < 1e-6f ? 1.0 : pow(2.0, tune / 12.0);
    dsp_process(&self->pool, self->table, self->sample, gain, rate, self->out_l, self->out_r, frames);

    // Monitors
    uint32_t active[(MAX_SLICES + 31) / 32];
    int64_t playhead = dsp_playheads(&self->pool, self->table, self->sample, active);
    for (int p = P_ACTIVE_0; p <= P_ACTIVE_LAST; p++) {
        uint32_t bits = 0;
        for (int b = 0; b < ACTIVE_BITS; b++) {
            int slice = (p - P_ACTIVE_0) * ACTIVE_BITS + b;
            if (slice < MAX_SLICES && (active[slice / 32] >> (slice % 32)) & 1)
                bits |= 1u << b;
        }
        if (self->mon[p])
            *self->mon[p] = (float)bits;
    }
    if (self->mon[P_PLAYHEAD]) {
        uint32_t source_frames = self->sample ? self->sample->source_frames : 0;
        *self->mon[P_PLAYHEAD] = playhead >= 0 && source_frames ? (float)((double)playhead / source_frames) : -1.0f;
    }
    if (self->mon[P_INSTANCE_ID])
        *self->mon[P_INSTANCE_ID] = (float)self->instance_id;
    if (self->mon[P_LIVE_VERSION])
        *self->mon[P_LIVE_VERSION] = (float)(self->live_version & 0xffffff);
    if (self->mon[P_EXPORT_COUNT])
        *self->mon[P_EXPORT_COUNT] = (float)(self->export_count & 0xffffff);

    // Save the slice map once edits settle
    if (self->save_countdown >= 0) {
        self->save_countdown -= frames;
        if (self->save_countdown < 0)
            request_simple(self, JOB_SAVE_MAP);
    }

    self->frame_clock += frames;
    lv2_atom_forge_pop(&self->forge, &notify_frame);
}

static void deactivate(LV2_Handle instance) {
    (void)instance;
}

static void cleanup(LV2_Handle instance) {
    Slicer* self = instance;
    // The worker isn't running any more: free everything here
    for (int i = 0; i < self->garbage_count; i++) {
        Garbage* g = &self->garbage[i];
        if (g->kind == GARBAGE_SAMPLE)
            sample_free(g->ptr);
        else
            free(g->ptr);
    }
    sample_free(self->sample);
    free(self->table);
    unlink(self->live_path);
    model_free(self->model);
    free(self->model);
    free(self->text);
    free(self);
}

// ----------------------------------------------------------------------------
// Worker
// ----------------------------------------------------------------------------

static int make_dir(const char* path) {
    char tmp[SLICER_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    return mkdir(tmp, 0755) && errno != EEXIST ? -1 : 0;
}

static void write_live_file(Slicer* self) {
    size_t len = slicemap_format(self->model, 1, self->text, self->text_size);
    if (!len || write_file_atomic(self->live_path, self->text, len))
        lv2_log_warning(&self->logger, "Slicer: can't write %s\n", self->live_path);
}

/* Save hand-edited slices as a slice map named by content. Returns its path
   in path ("" when there are no hand edits). */
static void save_map(Slicer* self, char* path, size_t size) {
    path[0] = 0;
    Model* model = self->model;
    if (!model->manual || !model->count)
        return;
    size_t len = slicemap_format(model, 0, self->text, self->text_size);
    if (!len)
        return;
    snprintf(path, size, "%s/%016llx.slices", self->maps_dir,
             (unsigned long long)slicer_hash(self->text, len));
    if (access(path, F_OK) == 0)
        return;
    if (make_dir(self->maps_dir) || write_file_atomic(path, self->text, len)) {
        lv2_log_error(&self->logger, "Slicer: can't save slice map %s\n", path);
        path[0] = 0;
    }
}

static void fill_selection(const Slicer* self, WorkResult* res) {
    const Model* model = self->model;
    res->flags |= FB_SELECTION;
    res->selected = model->selected;
    res->start_pct = res->end_pct = 0.0f;
    res->play_mode = DEFAULT_PLAY_MODE;
    if (model->count && model->source_frames) {
        const SliceDef* sl = &model->slices[model->selected];
        res->start_pct = (float)(100.0 * sl->start / model->source_frames);
        res->end_pct = (float)(100.0 * sl->end / model->source_frames);
        res->play_mode = sl->mode;
    }
}

static LV2_Worker_Status work(LV2_Handle instance, LV2_Worker_Respond_Function respond,
                              LV2_Worker_Respond_Handle handle, uint32_t size, const void* data) {
    Slicer* self = instance;
    Model* model = self->model;
    if (size < JOB_HEADER_SIZE)
        return LV2_WORKER_ERR_UNKNOWN;
    // Copy: the job may be truncated after the path
    Job job;
    memset(&job, 0, sizeof(job));
    memcpy(&job, data, size < sizeof(job) ? size : sizeof(job));
    job.path[sizeof(job.path) - 1] = 0;

    if (job.type == JOB_FREE) {
        if (job.u.garbage.kind == GARBAGE_SAMPLE)
            sample_free(job.u.garbage.ptr);
        else
            free(job.u.garbage.ptr);
        return LV2_WORKER_SUCCESS;
    }

    WorkResult* res = calloc(1, sizeof(WorkResult));
    if (!res)
        return LV2_WORKER_ERR_NO_SPACE;
    int slices_changed = 0;

    switch (job.type) {
        case JOB_LOAD_SAMPLE:
            if (!job.path[0]) {
                model_clear_sample(model);
                res->clear_sample = 1;
                slices_changed = 1;
            } else {
                Envelope envelope;
                if (sample_load(job.path, self->rate, &res->sample, &envelope) == 0) {
                    model_set_sample(model, job.path, res->sample->source_frames, &envelope);
                    slices_changed = 1;
                } else {
                    lv2_log_error(&self->logger, "Slicer: can't load sample %s\n", job.path);
                }
            }
            break;
        case JOB_LOAD_MAP:
            if (!job.path[0]) {
                model_clear_pending_map(model);
            } else {
                SliceMap* map = malloc(sizeof(SliceMap));
                if (map && slicemap_load(job.path, map) == 0) {
                    model_set_map(model, map);
                    // The map is already saved: don't report it back
                    snprintf(self->map_path, sizeof(self->map_path), "%s", job.path);
                    slices_changed = 1;
                } else {
                    lv2_log_error(&self->logger, "Slicer: can't read slice map %s\n", job.path);
                }
                free(map);
            }
            break;
        case JOB_PARAMS:
            model_set_params(model, &job.u.params);
            slices_changed = 1;
            break;
        case JOB_DETECT:
            model_detect(model);
            slices_changed = 1;
            break;
        case JOB_SELECT:
            model_select(model, job.u.ival);
            fill_selection(self, res);
            write_live_file(self);
            res->flags |= FB_LIVE;
            break;
        case JOB_SET_START:
            if (model->source_frames) {
                model_set_start(model, (uint32_t)llround(fmax(0.0, job.u.fval) * model->source_frames / 100.0));
                slices_changed = 1;
            }
            fill_selection(self, res);
            break;
        case JOB_SET_END:
            if (model->source_frames) {
                model_set_end(model, (uint32_t)llround(fmax(0.0, job.u.fval) * model->source_frames / 100.0));
                slices_changed = 1;
            }
            fill_selection(self, res);
            break;
        case JOB_SET_MODE:
            model_set_play_mode(model, job.u.ival);
            slices_changed = 1;
            break;
        case JOB_SAVE_MAP: {
            char path[SLICER_PATH_MAX];
            save_map(self, path, sizeof(path));
            if (strcmp(path, self->map_path)) {
                snprintf(self->map_path, sizeof(self->map_path), "%s", path);
                snprintf(res->map_path, sizeof(res->map_path), "%s", path);
                res->flags |= FB_MAP;
            }
            break;
        }
        case JOB_EXPORT: {
            SfzOptions options = {
                .gain_db = job.u.export_options.gain_db,
                .tune = job.u.export_options.tune,
                .polyphony = job.u.export_options.polyphony,
                .fade_seconds = FADE_FRAMES / self->rate,
                .base_note = model->params.base_note
            };
            if (sfz_export(model->path, model->slices, model->count, self->sfz_dir, &options,
                           model->last_export, sizeof(model->last_export),
                           model->last_export_error, sizeof(model->last_export_error))) {
                lv2_log_error(&self->logger, "Slicer: SFZ export failed: %s\n", model->last_export_error);
            }
            write_live_file(self);
            res->flags |= FB_LIVE | FB_EXPORT;
            break;
        }
        default:
            free(res);
            return LV2_WORKER_ERR_UNKNOWN;
    }

    if (slices_changed) {
        res->table = malloc(sizeof(SliceTable));
        if (res->table)
            model_fill_table(model, res->table);
        fill_selection(self, res);
        write_live_file(self);
        res->flags |= FB_LIVE | FB_SAVE;
    }

    if (respond(handle, sizeof(res), &res) != LV2_WORKER_SUCCESS) {
        lv2_log_error(&self->logger, "Slicer: worker response lost\n");
        sample_free(res->sample);
        free(res->table);
        free(res);
    }
    return LV2_WORKER_SUCCESS;
}

static LV2_Worker_Status work_response(LV2_Handle instance, uint32_t size, const void* data) {
    Slicer* self = instance;
    if (size != sizeof(WorkResult*))
        return LV2_WORKER_ERR_UNKNOWN;
    WorkResult* res = *(WorkResult* const*)data;

    if (res->sample || res->clear_sample) {
        dsp_reset(&self->pool);
        self->audition = 0;
        add_garbage(self, GARBAGE_SAMPLE, self->sample);
        self->sample = res->sample;
    }
    if (res->table) {
        add_garbage(self, GARBAGE_TABLE, self->table);
        self->table = res->table;
    }
    if (res->flags & FB_SELECTION) {
        self->fb_selected = res->selected;
        self->fb_start_pct = res->start_pct;
        self->fb_end_pct = res->end_pct;
        self->fb_play_mode = res->play_mode;
    }
    if (res->flags & FB_MAP)
        memcpy(self->fb_map_path, res->map_path, sizeof(self->fb_map_path));
    if (res->flags & FB_LIVE)
        self->live_version++;
    if (res->flags & FB_EXPORT)
        self->export_count++;
    if (res->flags & FB_SAVE)
        self->save_countdown = (int64_t)(self->rate * SAVE_DELAY_SECONDS);
    self->fb_flags |= res->flags & (FB_SELECTION | FB_MAP);
    add_garbage(self, GARBAGE_RESULT, res);
    return LV2_WORKER_SUCCESS;
}

static const void* extension_data(const char* uri) {
    static const LV2_Worker_Interface worker = {work, work_response, NULL};
    if (!strcmp(uri, LV2_WORKER__interface))
        return &worker;
    return NULL;
}

static const LV2_Descriptor descriptor = {
    SLICER_URI,
    instantiate,
    connect_port,
    activate,
    run,
    deactivate,
    cleanup,
    extension_data
};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return index == 0 ? &descriptor : NULL;
}
