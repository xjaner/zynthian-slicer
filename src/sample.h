/*
 * ******************************************************************
 * Zynthian Slicer LV2 plugin: sample loading (worker thread)
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

#ifndef SLICER_SAMPLE_H
#define SLICER_SAMPLE_H

#include "common.h"

/* Energy envelope of the source file, used to detect transients. env[i] is
   the RMS of the mono mix over frames [i * hop, (i + 1) * hop). */
typedef struct {
    double* env;
    uint32_t count;
    uint32_t hop;           // Frames per envelope value
    uint32_t source_rate;   // Samplerate of the source file
} Envelope;

/** @brief  Load an audio file, resampled to the host samplerate
    @param  path Audio file
    @param  host_rate Host samplerate
    @param  sample Receives the new sample (free with sample_free)
    @param  envelope Receives the energy envelope (free with envelope_free)
    @retval 0 on success, else -1
*/
int sample_load(const char* path, double host_rate, Sample** sample, Envelope* envelope);

void sample_free(Sample* sample);
void envelope_free(Envelope* envelope);

#endif
