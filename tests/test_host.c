/*
 * Minimal LV2 host that drives slicer.so the way jalv does: a threaded worker
 * with 4 KiB rings, responses delivered after run(), patch:Set messages on the
 * control port and feedback read from the notify port.
 *
 *   test_host PLUGIN_SO SAMPLE_WAV WORK_DIR
 */

#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/patch/patch.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#include "common.h"

#define RATE 48000.0
#define BLOCK 256
#define RING_SIZE 4096
#define SEQ_SIZE 8192

enum { P_GAIN, P_TUNE, P_BASE_NOTE, P_POLYPHONY, P_SLICE_MODE, P_SLICE_COUNT, P_SLICE_BEATS,
       P_SLICE_SUBDIVISION, P_THRESHOLD, P_MIN_GAP, P_ZOOM, P_AUDITION, P_CONTROL, P_NOTIFY, P_OUT_L, P_OUT_R,
       P_INSTANCE_ID, P_LIVE_VERSION, P_EXPORT_COUNT, P_PLAYHEAD, P_ACTIVE_0, P_COUNT = P_ACTIVE_0 + 6 };

static int failures = 0, checks = 0;
#define CHECK(cond) do { \
        checks++; \
        if (!(cond)) { \
            failures++; \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

// ----------------------------------------------------------------------------
// URID map
// ----------------------------------------------------------------------------

static char* uris[512];
static int n_uris = 0;
static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;

static LV2_URID map_uri(LV2_URID_Map_Handle h, const char* uri) {
    (void)h;
    pthread_mutex_lock(&map_lock);
    for (int i = 0; i < n_uris; i++)
        if (!strcmp(uris[i], uri)) {
            pthread_mutex_unlock(&map_lock);
            return i + 1;
        }
    uris[n_uris] = strdup(uri);
    LV2_URID id = ++n_uris;
    pthread_mutex_unlock(&map_lock);
    return id;
}

// ----------------------------------------------------------------------------
// Byte rings (size-prefixed packets, bounded like jalv's)
// ----------------------------------------------------------------------------

typedef struct {
    uint8_t buf[RING_SIZE];
    size_t used;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} Ring;

static int ring_write(Ring* r, uint32_t size, const void* data) {
    pthread_mutex_lock(&r->lock);
    if (r->used + sizeof(size) + size > RING_SIZE) {
        pthread_mutex_unlock(&r->lock);
        return -1;
    }
    memcpy(r->buf + r->used, &size, sizeof(size));
    memcpy(r->buf + r->used + sizeof(size), data, size);
    r->used += sizeof(size) + size;
    pthread_cond_signal(&r->cond);
    pthread_mutex_unlock(&r->lock);
    return 0;
}

/* Pop one packet into out (must hold RING_SIZE). Returns size, or 0 if empty. */
static uint32_t ring_read(Ring* r, void* out, int wait) {
    pthread_mutex_lock(&r->lock);
    while (wait && r->used == 0)
        pthread_cond_wait(&r->cond, &r->lock);
    if (r->used == 0) {
        pthread_mutex_unlock(&r->lock);
        return 0;
    }
    uint32_t size;
    memcpy(&size, r->buf, sizeof(size));
    memcpy(out, r->buf + sizeof(size), size);
    memmove(r->buf, r->buf + sizeof(size) + size, r->used - sizeof(size) - size);
    r->used -= sizeof(size) + size;
    pthread_mutex_unlock(&r->lock);
    return size;
}

// ----------------------------------------------------------------------------
// Host
// ----------------------------------------------------------------------------

typedef struct {
    const LV2_Descriptor* desc;
    LV2_Handle handle;
    const LV2_Worker_Interface* worker;
    Ring requests, responses;
    pthread_t thread;
    volatile int busy;      // Worker is processing a job
    volatile int quit;

    LV2_Atom_Forge forge;
    uint8_t control[SEQ_SIZE] __attribute__((aligned(8)));
    uint8_t notify[SEQ_SIZE] __attribute__((aligned(8)));
    LV2_Atom_Forge_Frame seq_frame;
    float out_l[BLOCK], out_r[BLOCK];
    float ports[P_COUNT];
    float peak;             // Output peak of the last cycle

    // Feedback received on the notify port
    float fb_slice, fb_start, fb_end, fb_mode, fb_detect, fb_export;
    int fb_count[8];
    char fb_map[1024];
    int fb_map_count;
} Host;

static LV2_URID u_patch_Set, u_patch_property, u_patch_value, u_midi, u_atom_Path, u_atom_Float, u_atom_Int,
    u_atom_URID, u_sample, u_slice_map, u_slice, u_slice_start, u_slice_end, u_play_mode, u_detect, u_export;

static LV2_Worker_Status schedule(LV2_Worker_Schedule_Handle h, uint32_t size, const void* data) {
    Host* host = h;
    return ring_write(&host->requests, size, data) ? LV2_WORKER_ERR_NO_SPACE : LV2_WORKER_SUCCESS;
}

static LV2_Worker_Status respond(LV2_Worker_Respond_Handle h, uint32_t size, const void* data) {
    Host* host = h;
    return ring_write(&host->responses, size, data) ? LV2_WORKER_ERR_NO_SPACE : LV2_WORKER_SUCCESS;
}

static void* worker_thread(void* arg) {
    Host* host = arg;
    uint8_t buf[RING_SIZE];
    while (!host->quit) {
        pthread_mutex_lock(&host->requests.lock);
        while (!host->quit && host->requests.used == 0)
            pthread_cond_wait(&host->requests.cond, &host->requests.lock);
        host->busy = 1;
        pthread_mutex_unlock(&host->requests.lock);
        if (host->quit)
            break;
        uint32_t size = ring_read(&host->requests, buf, 0);
        if (size)
            host->worker->work(host->handle, respond, host, size, buf);
        host->busy = 0;
    }
    return NULL;
}

static void begin_events(Host* host) {
    lv2_atom_forge_set_buffer(&host->forge, host->control, sizeof(host->control));
    lv2_atom_forge_sequence_head(&host->forge, &host->seq_frame, 0);
}

static void send_path(Host* host, LV2_URID key, const char* path) {
    LV2_Atom_Forge_Frame frame;
    lv2_atom_forge_frame_time(&host->forge, 0);
    lv2_atom_forge_object(&host->forge, &frame, 0, u_patch_Set);
    lv2_atom_forge_key(&host->forge, u_patch_property);
    lv2_atom_forge_urid(&host->forge, key);
    lv2_atom_forge_key(&host->forge, u_patch_value);
    lv2_atom_forge_path(&host->forge, path, (uint32_t)strlen(path));
    lv2_atom_forge_pop(&host->forge, &frame);
}

/* Numbers are sent as atom:Float, like jalv does for every property */
static void send_number(Host* host, LV2_URID key, float value) {
    LV2_Atom_Forge_Frame frame;
    lv2_atom_forge_frame_time(&host->forge, 0);
    lv2_atom_forge_object(&host->forge, &frame, 0, u_patch_Set);
    lv2_atom_forge_key(&host->forge, u_patch_property);
    lv2_atom_forge_urid(&host->forge, key);
    lv2_atom_forge_key(&host->forge, u_patch_value);
    lv2_atom_forge_float(&host->forge, value);
    lv2_atom_forge_pop(&host->forge, &frame);
}

static void send_midi(Host* host, uint32_t time, uint8_t status, uint8_t note, uint8_t velocity) {
    uint8_t msg[3] = {status, note, velocity};
    lv2_atom_forge_frame_time(&host->forge, time);
    lv2_atom_forge_atom(&host->forge, 3, u_midi);
    lv2_atom_forge_write(&host->forge, msg, 3);
}

static void read_feedback(Host* host) {
    const LV2_Atom_Sequence* seq = (const LV2_Atom_Sequence*)host->notify;
    LV2_ATOM_SEQUENCE_FOREACH(seq, ev) {
        const LV2_Atom_Object* obj = (const LV2_Atom_Object*)&ev->body;
        if (obj->body.otype != u_patch_Set)
            continue;
        const LV2_Atom* property = NULL;
        const LV2_Atom* value = NULL;
        lv2_atom_object_get(obj, u_patch_property, &property, u_patch_value, &value, 0);
        if (!property || !value)
            continue;
        LV2_URID key = ((const LV2_Atom_URID*)property)->body;
        if (key == u_slice_map) {
            snprintf(host->fb_map, sizeof(host->fb_map), "%s", (const char*)LV2_ATOM_BODY_CONST(value));
            host->fb_map_count++;
            continue;
        }
        float v = value->type == u_atom_Int ? (float)((const LV2_Atom_Int*)value)->body
                                            : ((const LV2_Atom_Float*)value)->body;
        if (key == u_slice)
            host->fb_slice = v;
        else if (key == u_slice_start)
            host->fb_start = v;
        else if (key == u_slice_end)
            host->fb_end = v;
        else if (key == u_play_mode)
            host->fb_mode = v;
        else if (key == u_detect)
            host->fb_detect = v;
        else if (key == u_export)
            host->fb_export = v;
    }
}

/* One cycle: run with the queued events, then deliver worker responses. */
static void cycle(Host* host) {
    lv2_atom_forge_pop(&host->forge, &host->seq_frame);
    LV2_Atom_Sequence* notify = (LV2_Atom_Sequence*)host->notify;
    notify->atom.type = 0;
    notify->atom.size = SEQ_SIZE - sizeof(LV2_Atom);
    host->desc->run(host->handle, BLOCK);
    read_feedback(host);
    host->peak = 0.0f;
    for (int i = 0; i < BLOCK; i++) {
        if (fabsf(host->out_l[i]) > host->peak)
            host->peak = fabsf(host->out_l[i]);
        if (fabsf(host->out_r[i]) > host->peak)
            host->peak = fabsf(host->out_r[i]);
    }
    uint8_t buf[RING_SIZE];
    uint32_t size;
    while ((size = ring_read(&host->responses, buf, 0)))
        host->worker->work_response(host->handle, size, buf);
    begin_events(host);
}

static void cycles(Host* host, int n) {
    for (int i = 0; i < n; i++)
        cycle(host);
}

/* Run cycles until the worker has been idle for a while. */
static void settle(Host* host) {
    int idle = 0;
    for (int i = 0; i < 20000 && idle < 8; i++) {
        cycle(host);
        int quiet = !host->busy && host->requests.used == 0 && host->responses.used == 0;
        idle = quiet ? idle + 1 : 0;
        if (!quiet)
            usleep(200);
    }
}

static void run_seconds(Host* host, double seconds) {
    int n = (int)(seconds * RATE / BLOCK) + 1;
    for (int i = 0; i < n; i++) {
        cycle(host);
        if (host->busy || host->requests.used)
            usleep(100);
    }
    settle(host);
}

static Host* host_new(const char* plugin_path) {
    static void* lib = NULL;
    if (!lib)
        lib = dlopen(plugin_path, RTLD_NOW);
    if (!lib) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        exit(1);
    }
    LV2_Descriptor_Function fn = (LV2_Descriptor_Function)dlsym(lib, "lv2_descriptor");
    Host* host = calloc(1, sizeof(Host));
    host->desc = fn(0);
    pthread_mutex_init(&host->requests.lock, NULL);
    pthread_cond_init(&host->requests.cond, NULL);
    pthread_mutex_init(&host->responses.lock, NULL);
    pthread_cond_init(&host->responses.cond, NULL);

    static LV2_URID_Map map = {NULL, map_uri};
    LV2_Worker_Schedule* sched = calloc(1, sizeof(LV2_Worker_Schedule));
    sched->handle = host;
    sched->schedule_work = schedule;
    LV2_Feature f_map = {LV2_URID__map, &map};
    LV2_Feature f_sched = {LV2_WORKER__schedule, sched};
    const LV2_Feature* features[] = {&f_map, &f_sched, NULL};
    host->handle = host->desc->instantiate(host->desc, RATE, "", features);
    if (!host->handle) {
        fprintf(stderr, "instantiate failed\n");
        exit(1);
    }
    host->worker = host->desc->extension_data(LV2_WORKER__interface);

    lv2_atom_forge_init(&host->forge, &map);
    host->desc->connect_port(host->handle, P_CONTROL, host->control);
    host->desc->connect_port(host->handle, P_NOTIFY, host->notify);
    host->desc->connect_port(host->handle, P_OUT_L, host->out_l);
    host->desc->connect_port(host->handle, P_OUT_R, host->out_r);
    for (int p = 0; p < P_COUNT; p++)
        if (p != P_CONTROL && p != P_NOTIFY && p != P_OUT_L && p != P_OUT_R)
            host->desc->connect_port(host->handle, p, &host->ports[p]);
    host->ports[P_GAIN] = 0;
    host->ports[P_TUNE] = 0;
    host->ports[P_BASE_NOTE] = 36;
    host->ports[P_POLYPHONY] = 16;
    host->ports[P_SLICE_MODE] = 0;
    host->ports[P_SLICE_COUNT] = 8;
    host->ports[P_SLICE_BEATS] = 4;
    host->ports[P_SLICE_SUBDIVISION] = 1;
    host->ports[P_THRESHOLD] = 50;
    host->ports[P_MIN_GAP] = 50;
    host->ports[P_ZOOM] = 1;
    host->ports[P_AUDITION] = 0;
    host->fb_slice = host->fb_start = host->fb_end = host->fb_mode = host->fb_detect = host->fb_export = -99;
    host->desc->activate(host->handle);
    pthread_create(&host->thread, NULL, worker_thread, host);
    begin_events(host);
    return host;
}

static void host_free(Host* host) {
    settle(host);
    pthread_mutex_lock(&host->requests.lock);
    host->quit = 1;
    pthread_cond_signal(&host->requests.cond);
    pthread_mutex_unlock(&host->requests.lock);
    pthread_join(host->thread, NULL);
    host->desc->deactivate(host->handle);
    host->desc->cleanup(host->handle);
    free(host);
}

// ----------------------------------------------------------------------------
// Live slice file
// ----------------------------------------------------------------------------

typedef struct {
    int count, manual, selected, base_note;
    unsigned starts[MAX_SLICES], ends[MAX_SLICES];
    char modes[MAX_SLICES][16];
    char sample[1024], export_path[1024], export_error[256];
} Live;

static int read_live(const Host* host, const char* dir, Live* live) {
    memset(live, 0, sizeof(*live));
    char path[1200];
    snprintf(path, sizeof(path), "%s/zynthian-slicer-%u.slices", dir, (unsigned)host->ports[P_INSTANCE_ID]);
    FILE* f = fopen(path, "r");
    if (!f)
        return -1;
    char line[1200];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "slice=", 6) && live->count < MAX_SLICES) {
            sscanf(line + 6, "%u %u %15s", &live->starts[live->count], &live->ends[live->count], live->modes[live->count]);
            live->count++;
        } else if (!strncmp(line, "manual=", 7)) {
            live->manual = atoi(line + 7);
        } else if (!strncmp(line, "selected=", 9)) {
            live->selected = atoi(line + 9);
        } else if (!strncmp(line, "base_note=", 10)) {
            live->base_note = atoi(line + 10);
        } else if (!strncmp(line, "sample=", 7)) {
            snprintf(live->sample, sizeof(live->sample), "%s", line + 7);
        } else if (!strncmp(line, "export=", 7)) {
            snprintf(live->export_path, sizeof(live->export_path), "%s", line + 7);
        } else if (!strncmp(line, "export_error=", 13)) {
            snprintf(live->export_error, sizeof(live->export_error), "%s", line + 13);
        }
    }
    fclose(f);
    return 0;
}

static int file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int active_bit(const Host* host, int slice) {
    unsigned bits = (unsigned)host->ports[P_ACTIVE_0 + slice / 24];
    return (bits >> (slice % 24)) & 1;
}

// ----------------------------------------------------------------------------
// Tests
// ----------------------------------------------------------------------------

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: test_host PLUGIN_SO SAMPLE_WAV WORK_DIR\n");
        return 2;
    }
    const char* plugin = argv[1];
    const char* wav = argv[2];
    const char* work = argv[3];
    char live_dir[1024], data_dir[1024], path[2048];
    snprintf(live_dir, sizeof(live_dir), "%s/live", work);
    snprintf(data_dir, sizeof(data_dir), "%s/mydata", work);
    mkdir(work, 0755);
    mkdir(live_dir, 0755);
    mkdir(data_dir, 0755);
    setenv("ZYNTHIAN_SLICER_LIVE_DIR", live_dir, 1);
    setenv("ZYNTHIAN_MY_DATA_DIR", data_dir, 1);

    LV2_URID_Map map = {NULL, map_uri};
    u_patch_Set = map_uri(NULL, LV2_PATCH__Set);
    u_patch_property = map_uri(NULL, LV2_PATCH__property);
    u_patch_value = map_uri(NULL, LV2_PATCH__value);
    u_midi = map_uri(NULL, LV2_MIDI__MidiEvent);
    u_atom_Path = map_uri(NULL, LV2_ATOM__Path);
    u_atom_Float = map_uri(NULL, LV2_ATOM__Float);
    u_atom_Int = map_uri(NULL, LV2_ATOM__Int);
    u_atom_URID = map_uri(NULL, LV2_ATOM__URID);
    u_sample = map_uri(NULL, SLICER_URI "#sample");
    u_slice_map = map_uri(NULL, SLICER_URI "#slice_map");
    u_slice = map_uri(NULL, SLICER_URI "#slice");
    u_slice_start = map_uri(NULL, SLICER_URI "#slice_start");
    u_slice_end = map_uri(NULL, SLICER_URI "#slice_end");
    u_play_mode = map_uri(NULL, SLICER_URI "#play_mode");
    u_detect = map_uri(NULL, SLICER_URI "#detect");
    u_export = map_uri(NULL, SLICER_URI "#export_sfz");
    (void)map;

    Live live;

    // --- Instance A: load, play, slice, edit, save -------------------------
    Host* a = host_new(plugin);
    cycles(a, 4);
    CHECK(a->ports[P_INSTANCE_ID] > 0);
    CHECK(a->ports[P_PLAYHEAD] == -1.0f);
    CHECK(a->peak == 0.0f);

    send_path(a, u_sample, wav);
    settle(a);
    CHECK(read_live(a, live_dir, &live) == 0);
    CHECK(live.count == 4 && !live.manual && !strcmp(live.sample, wav));
    CHECK(a->ports[P_LIVE_VERSION] > 0);
    CHECK(a->fb_slice == 0 && a->fb_start == 0 && a->fb_end == 250 && a->fb_mode == PLAY_GATE);

    // Play the first slice
    send_midi(a, 10, LV2_MIDI_MSG_NOTE_ON, 36, 100);
    cycles(a, 3);
    CHECK(a->peak > 0.0f);
    CHECK(active_bit(a, 0) && !active_bit(a, 1));
    CHECK(a->ports[P_PLAYHEAD] > 0.0f && a->ports[P_PLAYHEAD] < 0.25f);
    send_midi(a, 0, LV2_MIDI_MSG_NOTE_OFF, 36, 0);
    cycles(a, 2);
    CHECK(a->peak == 0.0f && !active_bit(a, 0)); // Gate: faded out within 64 frames
    // Unmapped notes are silent
    send_midi(a, 0, LV2_MIDI_MSG_NOTE_ON, 40, 100);
    cycles(a, 2);
    CHECK(a->peak == 0.0f);

    // Grid of 8
    a->ports[P_SLICE_MODE] = SLICE_GRID;
    a->ports[P_SLICE_COUNT] = 8;
    settle(a);
    read_live(a, live_dir, &live);
    CHECK(live.count == 8 && !live.manual);

    // Edits right after loading are ignored (a snapshot being restored)
    send_path(a, u_sample, wav);
    send_number(a, u_slice, 1);
    send_number(a, u_slice_start, 200);
    settle(a);
    read_live(a, live_dir, &live);
    CHECK(!live.manual && a->fb_slice == 1 && a->fb_start == 125);
    run_seconds(a, 0.6);

    // Select and edit
    send_number(a, u_slice, 3);
    settle(a);
    CHECK(a->fb_slice == 3 && a->fb_start == 375 && a->fb_end == 500);
    read_live(a, live_dir, &live);
    CHECK(live.selected == 3);
    send_number(a, u_slice, 200); // Clamped
    settle(a);
    CHECK(a->fb_slice == 7);
    send_number(a, u_slice, 3);
    send_number(a, u_slice_start, 400);
    send_number(a, u_play_mode, PLAY_LOOP);
    settle(a);
    CHECK(a->fb_start == 400 && a->fb_mode == PLAY_LOOP);
    read_live(a, live_dir, &live);
    CHECK(live.manual && live.count == 8 && live.ends[2] == live.starts[3] && !strcmp(live.modes[3], "loop"));
    send_number(a, u_slice_end, 990); // Limited by the next slice's end
    settle(a);
    CHECK(a->fb_end == 625); // 55124 of 88200 frames

    // The slice map is saved once edits settle
    CHECK(a->fb_map_count == 0);
    run_seconds(a, 1.2);
    CHECK(a->fb_map_count == 1 && strstr(a->fb_map, "/mydata/slicer/") && file_exists(a->fb_map));
    char saved_map[1024];
    snprintf(saved_map, sizeof(saved_map), "%s", a->fb_map);
    Live edited;
    read_live(a, live_dir, &edited);

    // Analysis changes keep hand edits
    a->ports[P_SLICE_COUNT] = 4;
    settle(a);
    read_live(a, live_dir, &live);
    CHECK(live.count == 8 && live.manual);

    // Audition slice 5 through the port, as the widget does
    a->ports[P_AUDITION] = 6;
    cycles(a, 2);
    CHECK(active_bit(a, 5) && a->peak > 0.0f);
    a->ports[P_AUDITION] = 0;
    cycles(a, 2);
    CHECK(!active_bit(a, 5));

    // --- Instance B: restore from snapshot values (map before sample) ------
    Host* b = host_new(plugin);
    b->ports[P_SLICE_MODE] = SLICE_GRID;
    b->ports[P_SLICE_COUNT] = 4;
    send_path(b, u_slice_map, saved_map);
    send_path(b, u_sample, wav);
    settle(b);
    read_live(b, live_dir, &live);
    CHECK(live.count == edited.count && live.manual);
    CHECK(!memcmp(live.starts, edited.starts, sizeof(live.starts)) && !strcmp(live.modes[3], "loop"));
    run_seconds(b, 1.2);
    CHECK(b->fb_map_count == 0); // Restored map isn't reported back
    host_free(b);

    // --- Instance C: sample before map, as a snapshot may send them --------
    Host* c = host_new(plugin);
    c->ports[P_SLICE_MODE] = SLICE_GRID;
    send_path(c, u_sample, wav);
    cycles(c, 1);
    send_path(c, u_slice_map, saved_map);
    settle(c);
    read_live(c, live_dir, &live);
    CHECK(live.count == edited.count && live.manual && !memcmp(live.starts, edited.starts, sizeof(live.starts)));
    host_free(c);

    // --- Instance D: snapshot values in the worst order --------------------
    // Edits and selection before the sample, all in one burst
    Host* d = host_new(plugin);
    d->ports[P_SLICE_MODE] = SLICE_GRID;
    d->ports[P_SLICE_COUNT] = 4;
    send_number(d, u_slice_start, 100);
    send_number(d, u_play_mode, PLAY_ONE_SHOT);
    send_number(d, u_slice, 3);
    send_path(d, u_sample, wav);
    send_number(d, u_slice_end, 700);
    send_path(d, u_slice_map, saved_map);
    settle(d);
    read_live(d, live_dir, &live);
    CHECK(live.count == edited.count && live.manual && live.selected == 3);
    CHECK(!memcmp(live.starts, edited.starts, sizeof(live.starts)) && !memcmp(live.ends, edited.ends, sizeof(live.ends)));
    CHECK(!strcmp(live.modes[3], "loop"));
    CHECK(d->fb_slice == 3 && d->fb_start == 400 && d->fb_mode == PLAY_LOOP);
    run_seconds(d, 1.2);
    CHECK(d->fb_map_count == 0);
    host_free(d);

    // --- Back to A: detect discards edits and clears the map ---------------
    send_number(a, u_detect, 1);
    settle(a);
    CHECK(a->fb_detect == 0.0f);
    read_live(a, live_dir, &live);
    CHECK(live.count == 4 && !live.manual);
    run_seconds(a, 1.2);
    CHECK(a->fb_map_count == 2 && a->fb_map[0] == 0);

    // Tune: an octave up plays the slice in half the time
    a->ports[P_TUNE] = 12;
    send_midi(a, 0, LV2_MIDI_MSG_NOTE_ON, 36, 127);
    cycles(a, 2);
    float p1 = a->ports[P_PLAYHEAD];
    cycles(a, 10);
    float p2 = a->ports[P_PLAYHEAD];
    CHECK(fabsf((p2 - p1) - 10.0f * BLOCK * 2.0f / 96000.0f) < 0.002f); // 2 s file => 96000 frames at 48 kHz
    send_midi(a, 0, LV2_MIDI_MSG_CONTROLLER, LV2_MIDI_CTL_ALL_NOTES_OFF, 0);
    cycles(a, 2);
    CHECK(a->peak == 0.0f);
    a->ports[P_TUNE] = 0;

    // Base note moves the slices up
    a->ports[P_BASE_NOTE] = 60;
    settle(a);
    send_midi(a, 0, LV2_MIDI_MSG_NOTE_ON, 36, 127);
    cycles(a, 2);
    CHECK(a->peak == 0.0f);
    send_midi(a, 0, LV2_MIDI_MSG_NOTE_ON, 61, 127);
    cycles(a, 2);
    CHECK(active_bit(a, 1));
    send_midi(a, 0, LV2_MIDI_MSG_NOTE_ON, 0, 127); // Note 0 stops everything
    cycles(a, 2);
    CHECK(a->peak == 0.0f);

    // Export SFZ
    float exports = a->ports[P_EXPORT_COUNT];
    send_number(a, u_export, 1);
    settle(a);
    CHECK(a->fb_export == 0.0f);
    CHECK(a->ports[P_EXPORT_COUNT] == exports + 1);
    read_live(a, live_dir, &live);
    CHECK(live.export_path[0] && !live.export_error[0] && file_exists(live.export_path));
    snprintf(path, sizeof(path), "%s/soundfonts/sfz", data_dir);
    CHECK(strncmp(live.export_path, path, strlen(path)) == 0);
    // Re-exporting the same sample reuses its folder
    char first_export[1024];
    snprintf(first_export, sizeof(first_export), "%s", live.export_path);
    send_number(a, u_export, 1);
    settle(a);
    read_live(a, live_dir, &live);
    CHECK(!strcmp(live.export_path, first_export));

    // Missing file: keeps the current sample
    send_path(a, u_sample, "/nonexistent/file.wav");
    settle(a);
    read_live(a, live_dir, &live);
    CHECK(live.count == 4 && !strcmp(live.sample, wav));

    // Empty path ("_" from Zynthian) unloads
    send_path(a, u_sample, "_");
    settle(a);
    read_live(a, live_dir, &live);
    CHECK(live.count == 0 && !live.sample[0]);
    send_midi(a, 0, LV2_MIDI_MSG_NOTE_ON, 60, 127);
    cycles(a, 2);
    CHECK(a->peak == 0.0f);

    // Export without slices reports an error
    send_number(a, u_export, 1);
    settle(a);
    read_live(a, live_dir, &live);
    CHECK(live.export_error[0] && !live.export_path[0]);

    // Burst of edits larger than the worker ring: nothing is lost
    send_path(a, u_sample, wav);
    settle(a);
    for (int i = 0; i < 300; i++) {
        send_number(a, u_slice, i % 4);
        if (i % 8 == 0)
            cycle(a);
    }
    send_number(a, u_slice, 2);
    settle(a);
    CHECK(a->fb_slice == 2);

    snprintf(path, sizeof(path), "%s/zynthian-slicer-%u.slices", live_dir, (unsigned)a->ports[P_INSTANCE_ID]);
    host_free(a);
    CHECK(!file_exists(path)); // Live file removed on cleanup

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
