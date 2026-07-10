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
 * WebUI phone call-audio settings: GET current config + apply a settings save.
 * Follows the Home Assistant tool pattern (custom GET message + set_config
 * special-case).  The config is a tool-owned struct, so it does not round-trip
 * through dawn_config_t / SETTINGS_SCHEMA.
 */

#include "webui/webui_phone_config.h"

#include <json-c/json.h>

#include "audio/phone_apm.h"
#include "logging.h"
#include "tools/phone_audio_bridge.h"
#include "tools/phone_audio_config.h"
#include "tools/phone_tool.h"
#include "webui/webui_internal.h"

/* Serialize one direction's APM knobs.  echo_cancel only for the uplink (the
 * downlink has no reverse stream). */
static struct json_object *apm_to_json(const phone_apm_config_t *c, bool include_echo) {
   struct json_object *o = json_object_new_object();
   json_object_object_add(o, "agc", json_object_new_boolean(c->agc_enabled));
   json_object_object_add(o, "fixed_gain_db", json_object_new_double(c->fixed_gain_db));
   json_object_object_add(o, "agc_ramp_db_per_s",
                          json_object_new_double(c->max_gain_change_db_per_s));
   json_object_object_add(o, "max_output_noise_dbfs",
                          json_object_new_double(c->max_output_noise_dbfs));
   json_object_object_add(o, "ns_level",
                          json_object_new_string(phone_ns_level_to_string(c->ns_level)));
   json_object_object_add(o, "high_pass", json_object_new_boolean(c->high_pass));
   if (include_echo) {
      json_object_object_add(o, "echo_cancel", json_object_new_boolean(c->echo_cancel));
   }
   return o;
}

/* Overlay whatever fields are present in @sub onto @out (absent keys unchanged). */
static void apm_from_json(struct json_object *sub, phone_apm_config_t *out, bool include_echo) {
   struct json_object *v;
   if (!sub) {
      return;
   }
   if (json_object_object_get_ex(sub, "agc", &v)) {
      out->agc_enabled = json_object_get_boolean(v);
   }
   if (json_object_object_get_ex(sub, "fixed_gain_db", &v)) {
      out->fixed_gain_db = (float)json_object_get_double(v);
   }
   if (json_object_object_get_ex(sub, "agc_ramp_db_per_s", &v)) {
      out->max_gain_change_db_per_s = (float)json_object_get_double(v);
   }
   if (json_object_object_get_ex(sub, "max_output_noise_dbfs", &v)) {
      out->max_output_noise_dbfs = (float)json_object_get_double(v);
   }
   if (json_object_object_get_ex(sub, "ns_level", &v)) {
      out->ns_level = phone_ns_level_from_string(json_object_get_string(v));
   }
   if (json_object_object_get_ex(sub, "high_pass", &v)) {
      out->high_pass = json_object_get_boolean(v);
   }
   if (include_echo && json_object_object_get_ex(sub, "echo_cancel", &v)) {
      out->echo_cancel = json_object_get_boolean(v);
   }
}

void handle_phone_audio_config_get(ws_connection_t *conn) {
   if (!conn_require_admin(conn)) {
      return;
   }
   phone_audio_config_t cfg;
   phone_tool_get_audio_config(&cfg);

   struct json_object *response = json_object_new_object();
   json_object_object_add(response, "type", json_object_new_string("phone_audio_config_response"));
   struct json_object *p = json_object_new_object();

   /* webrtc_available: on a non-WebRTC build every APM control is inert (the panel
    * should disable/annotate them). bridge_active: whether a call is live now. */
   json_object_object_add(p, "webrtc_available", json_object_new_boolean(phone_apm_available()));
   json_object_object_add(p, "bridge_active", json_object_new_boolean(phone_bridge_is_active()));
   json_object_object_add(p, "uplink", apm_to_json(&cfg.uplink, true));
   json_object_object_add(p, "downlink", apm_to_json(&cfg.downlink, false));
   json_object_object_add(p, "downlink_apm", json_object_new_boolean(cfg.downlink_use_apm));
   json_object_object_add(p, "downlink_gain", json_object_new_double(cfg.downlink_gain));

   json_object_object_add(response, "payload", p);
   send_json_response(conn, response);
   json_object_put(response);
}

void webui_phone_apply_config(ws_connection_t *conn, struct json_object *phone_section) {
   if (!phone_section) {
      return;
   }
   /* Start from the current config so unspecified fields (and pcm_port, which the
    * panel never sends) are preserved, then overlay the payload. */
   phone_audio_config_t cfg;
   phone_tool_get_audio_config(&cfg);

   struct json_object *sub;
   if (json_object_object_get_ex(phone_section, "uplink", &sub)) {
      apm_from_json(sub, &cfg.uplink, /*include_echo=*/true);
   }
   if (json_object_object_get_ex(phone_section, "downlink", &sub)) {
      apm_from_json(sub, &cfg.downlink, /*include_echo=*/false);
   }
   struct json_object *v;
   if (json_object_object_get_ex(phone_section, "downlink_apm", &v)) {
      cfg.downlink_use_apm = json_object_get_boolean(v);
   }
   if (json_object_object_get_ex(phone_section, "downlink_gain", &v)) {
      cfg.downlink_gain = (float)json_object_get_double(v);
   }
   /* The panel's downlink "Use auto gain" toggle IS the AGC switch, so keep the
    * inner AGC flag in lockstep — otherwise the panel could show AGC on while
    * AGC2 is actually off (APM running HPF/NS only). */
   cfg.downlink.agc_enabled = cfg.downlink_use_apm;

   /* Clamps, persists to module state, and live-applies to a call in progress. */
   phone_tool_update_config(&cfg);
   OLOG_INFO("WebUI: phone audio config updated");

   /* Echo the stored (clamped) config back so the panel reflects e.g. 60 -> 49. */
   handle_phone_audio_config_get(conn);
}
