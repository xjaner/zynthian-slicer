/*
 * ******************************************************************
 * Zynthian Slicer LV2 plugin: slicing model (worker thread only)
 *
 * Holds the loaded sample's slices and applies every change to them:
 * automatic slicing (beats, grid, transients), hand edits and saved slice
 * maps. Ported from the Python slicer engine (zynthian_slicer_analysis.py and
 * zynthian_engine_slicer.py).
 *
 * ******************************************************************
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or any later version.
 *
 * For a full copy of the GNU General Public License see the LICENSE.txt file.
 *
 * ******************************************************************
 */

#ifndef SLICER_MODEL_H
#define SLICER_MODEL_H

#include <stddef.h>

#include "common.h"
#include "sample.h"

typedef struct {
    int slice_mode;     // SLICE_MODE
    int slice_count;    // Grid slices
    int beats;          // Beats in the sample
    int subdivision;    // Slices per beat
    float threshold;    // Transient sensitivity 0..100 (higher detects fewer)
    float min_gap_ms;   // Minimum distance between transients
    int base_note;      // MIDI note of the first slice
} SliceParams;

/* Slice map: the contents of a .slices file */
typedef struct {
    char sample[SLICER_PATH_MAX];
    uint32_t frames;
    int manual;
    int selected;
    uint32_t count;
    SliceDef slices[MAX_SLICES];
} SliceMap;

typedef struct {
    // Loaded sample ("" when none)
    char path[SLICER_PATH_MAX];
    uint32_t source_frames;
    Envelope envelope;

    // Slices, contiguous, in source frames
    uint32_t count;
    SliceDef slices[MAX_SLICES];
    int manual;             // 1 once the slices were edited by hand
    int selected;           // Slice shown on the Edit page

    SliceParams params;

    // Slice map received before its sample was loaded
    int has_pending_map;
    SliceMap pending_map;

    // Result of the last SFZ export, for the widget
    char last_export[SLICER_PATH_MAX];
    char last_export_error[128];
} Model;

// ----------------------------------------------------------------------------
// Pure slicing functions
// ----------------------------------------------------------------------------

/** @brief  Quantity of slices that fit between base_note and the top MIDI note */
int slicer_max_slices(int base_note);

/** @brief  Split frames into count equal, contiguous slices
    @retval Quantity of slices written to out
*/
uint32_t slicer_even(uint32_t frames, int count, SliceDef* out);

/** @brief  Detect transients with an energy-flux onset detector
    @param  threshold Sensitivity 0..100 (higher detects fewer onsets)
    @param  min_gap_ms Minimum distance between onsets
    @retval Quantity of contiguous slices written to out (0 if nothing found)
*/
uint32_t slicer_transients(const Envelope* envelope, uint32_t source_frames,
                           float threshold, float min_gap_ms, SliceDef* out);

/** @brief  Make slices a strict contiguous partition of the sample
    Slice starts are authoritative: invalid slices are skipped, starts are
    made strictly increasing and every end is set to the next slice's start.
    Slices that don't fit above base_note are dropped.
    @retval Quantity of slices written to out (may be in == out)
*/
uint32_t slicer_normalize(const SliceDef* in, uint32_t count, uint32_t frames,
                          int base_note, SliceDef* out);

// ----------------------------------------------------------------------------
// Model
// ----------------------------------------------------------------------------

void model_init(Model* model);
void model_free(Model* model);

/** @brief  Use a newly loaded sample (takes ownership of the envelope)
    Same file as before => keeps the slices. A pending slice map for this file
    => uses it. Otherwise => slices automatically.
*/
void model_set_sample(Model* model, const char* path, uint32_t frames, Envelope* envelope);

/** @brief  Remove the sample and its slices */
void model_clear_sample(Model* model);

/** @brief  Apply a saved slice map, now if its sample is loaded, else when it is */
void model_set_map(Model* model, const SliceMap* map);

/** @brief  Forget a slice map waiting for its sample */
void model_clear_pending_map(Model* model);

/** @brief  Update the slicing parameters, slicing again if allowed
    Analysis changes only slice again while there are no hand edits and the
    mode isn't manual. A base note change drops the slices that no longer fit.
*/
void model_set_params(Model* model, const SliceParams* params);

/** @brief  Slice again with the current parameters, discarding hand edits
    In manual mode the existing slices are kept.
*/
void model_detect(Model* model);

void model_select(Model* model, int index);

/** @brief  Move the selected slice's start or end (in source frames)
    Slices stay contiguous: moving a start also moves the previous slice's
    end, moving an end also moves the next slice's start.
*/
void model_set_start(Model* model, uint32_t frame);
void model_set_end(Model* model, uint32_t frame);

/** @brief  Set the selected slice's play mode */
void model_set_play_mode(Model* model, int mode);

/** @brief  Copy the slices into a table for the audio thread */
void model_fill_table(const Model* model, SliceTable* table);

// ----------------------------------------------------------------------------
// Slice map files
// ----------------------------------------------------------------------------

/** @brief  Format the model as slice map text
    @param  live 1 to add display-only fields (selection, base note, export)
    @retval Length of the text, or 0 if it doesn't fit
*/
size_t slicemap_format(const Model* model, int live, char* buf, size_t size);

/** @brief  Parse slice map text. Returns 0 on success. */
int slicemap_parse(const char* text, SliceMap* map);

/** @brief  Read a slice map file. Returns 0 on success. */
int slicemap_load(const char* path, SliceMap* map);

/** @brief  Write text to a file atomically (via a temporary file). Returns 0 on success. */
int write_file_atomic(const char* path, const char* text, size_t len);

/** @brief  FNV-1a 64-bit hash, used to name saved slice maps by content */
uint64_t slicer_hash(const char* text, size_t len);

const char* play_mode_name(int mode);

#endif
