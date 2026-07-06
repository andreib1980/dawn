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
 * the project author(s).
 *
 * Phone Audio Bridge — full-duplex PCM between the SIM7600G-H modem USB audio
 * port (raw 16 kHz S16LE mono on /dev/ttyUSB4) and the local handset (the
 * primary mic + speaker).  A single lockstep thread, paced by the blocking mic
 * capture, writes one uplink frame and reads the matching amount of downlink per
 * cycle — the SIM7600 USB PCM is synchronous full-duplex, so read and write must
 * stay balanced or the modem stops draining uplink.
 */

#ifndef PHONE_AUDIO_BRIDGE_H
#define PHONE_AUDIO_BRIDGE_H

#include <stdbool.h>

/**
 * @brief Start the local-handset audio bridge.
 *
 * Opens the modem PCM port (@p pcm_port, e.g. "/dev/ttyUSB4") and the local
 * capture/playback devices, then spawns the single lockstep bridge thread.  Must
 * be called only after ECHO has armed the modem PCM (i.e. on the `pcm_ready`
 * event), so data is already flowing.
 *
 * Serialized with phone_bridge_stop() via an internal mutex.  Idempotent: if the
 * bridge is already running this is a no-op that returns SUCCESS.
 *
 * @param pcm_port Modem USB audio device node (raw 16 kHz S16LE mono).
 * @return SUCCESS if the bridge is running on return, FAILURE otherwise.
 */
int phone_bridge_start(const char *pcm_port);

/**
 * @brief Stop the audio bridge.
 *
 * Signals the bridge thread, joins it, and closes the PCM port and audio
 * streams.  Bounded: the thread exits within one blocking mic-capture period, so
 * this cannot hang.  Safe to call when the bridge is not running.
 */
void phone_bridge_stop(void);

/** @brief True while the bridge thread is running. */
bool phone_bridge_is_active(void);

#endif /* PHONE_AUDIO_BRIDGE_H */
