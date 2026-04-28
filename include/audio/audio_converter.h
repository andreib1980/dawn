/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * DAWN Audio Converter - Configurable Stereo Output
 *
 * Provides high-quality audio conversion to a configurable output format for
 * consistent audio quality across all playback sources (TTS, music, etc).
 *
 * Output Format (configurable via dawn.toml):
 *   - Sample Rate: 44100 Hz (default) or 48000 Hz
 *   - Channels: 2 (stereo, required for dmix compatibility)
 *   - Format: S16_LE (signed 16-bit little-endian)
 *
 * Benefits:
 *   - Consistent quality: all audio goes through same high-quality resampler
 *   - dmix compatibility: stereo output works with ALSA dmix for mixing
 *   - No hidden conversions: ALSA/Pulse pass-through at native rate
 *   - 44100 Hz avoids resampling for most music (CD quality)
 *
 * Usage:
 *   1. Create converter with audio_converter_create()
 *   2. Convert audio with audio_converter_process()
 *   3. Destroy with audio_converter_destroy()
 */

#ifndef AUDIO_CONVERTER_H
#define AUDIO_CONVERTER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default output format constants (used if config not loaded) */
#define AUDIO_CONV_DEFAULT_OUTPUT_RATE 44100
#define AUDIO_CONV_DEFAULT_OUTPUT_CHANNELS 2
#define AUDIO_CONV_MAX_INPUT_FRAMES 8192

/**
 * @brief Get the configured output sample rate
 *
 * Returns the output rate from config if available, otherwise the default.
 * Call this instead of using AUDIO_CONV_DEFAULT_OUTPUT_RATE directly.
 *
 * @return Output sample rate (typically 44100 or 48000)
 */
unsigned int audio_conv_get_output_rate(void);

/**
 * @brief Get the configured output channel count
 *
 * Returns the output channels from config if available, otherwise the default.
 * Call this instead of using AUDIO_CONV_DEFAULT_OUTPUT_CHANNELS directly.
 *
 * @return Output channel count (typically 2 for stereo)
 */
unsigned int audio_conv_get_output_channels(void);

/**
 * @brief Opaque converter handle
 */
typedef struct audio_converter audio_converter_t;

/**
 * @brief Input audio format specification
 */
typedef struct {
   unsigned int sample_rate; /**< Input sample rate (e.g., 22050, 44100) */
   unsigned int channels;    /**< Input channels (1=mono, 2=stereo) */
} audio_converter_params_t;

/**
 * @brief Create an audio converter for the specified input format
 *
 * Creates a converter that transforms audio from the input format to
 * the configured output format (default: 44100Hz stereo).
 *
 * @param params Input audio parameters
 * @return Converter handle on success, NULL on failure
 */
audio_converter_t *audio_converter_create(const audio_converter_params_t *params);

/**
 * @brief Create an audio converter with explicit output parameters
 *
 * Creates a converter that transforms audio from the input format to
 * the specified output format. Use this when you know the actual hardware
 * rate (e.g., from ALSA hw_params) which may differ from config.
 *
 * @param params Input audio parameters
 * @param output_rate Actual output sample rate (e.g., from ALSA hw_params)
 * @param output_channels Actual output channel count
 * @return Converter handle on success, NULL on failure
 */
audio_converter_t *audio_converter_create_ex(const audio_converter_params_t *params,
                                             unsigned int output_rate,
                                             unsigned int output_channels);

/**
 * @brief Destroy an audio converter and free resources
 *
 * @param conv Converter handle (safe to pass NULL)
 */
void audio_converter_destroy(audio_converter_t *conv);

/**
 * @brief Get maximum output frames for a given input size
 *
 * Use this to allocate output buffer before calling audio_converter_process().
 *
 * @param conv Converter handle
 * @param input_frames Number of input frames
 * @return Maximum number of output frames
 */
size_t audio_converter_max_output_frames(audio_converter_t *conv, size_t input_frames);

/**
 * @brief Convert audio to configured stereo format
 *
 * Converts input audio (any supported rate/channels) to configured output S16_LE.
 * Uses libsamplerate for high-quality resampling.
 *
 * @param conv Converter handle
 * @param input Input samples (S16_LE, interleaved if stereo)
 * @param input_frames Number of input frames
 * @param output Output buffer (must be large enough - use audio_converter_max_output_frames())
 * @param output_max_frames Maximum frames the output buffer can hold
 * @param frames_out Output: number of output frames written
 * @return 0 on success, 1 on error
 */
int audio_converter_process(audio_converter_t *conv,
                            const int16_t *input,
                            size_t input_frames,
                            int16_t *output,
                            size_t output_max_frames,
                            size_t *frames_out);

/**
 * @brief Reset converter state (e.g., between tracks)
 *
 * Clears any internal resampler state. Call between unrelated audio segments.
 *
 * @param conv Converter handle
 */
void audio_converter_reset(audio_converter_t *conv);

/**
 * @brief Check if conversion is needed for given parameters
 *
 * Returns true if the input format differs from configured output.
 * Can be used to skip conversion overhead when input matches output.
 *
 * @param params Input parameters to check
 * @return true if conversion needed, false if passthrough
 */
int audio_converter_needed(const audio_converter_params_t *params);

/**
 * @brief Check if conversion is needed against explicit output parameters
 *
 * Returns true if the input format differs from specified output.
 * Use this when you know the actual hardware rate (e.g., from ALSA hw_params).
 *
 * @param params Input parameters to check
 * @param output_rate Actual output sample rate
 * @param output_channels Actual output channel count
 * @return true if conversion needed, false if passthrough
 */
int audio_converter_needed_ex(const audio_converter_params_t *params,
                              unsigned int output_rate,
                              unsigned int output_channels);

/**
 * @brief Get the resampling ratio for a converter
 *
 * @param conv Converter handle
 * @return Ratio of output_rate/input_rate
 */
double audio_converter_get_ratio(audio_converter_t *conv);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_CONVERTER_H */
