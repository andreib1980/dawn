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
 * phone_audio_config — parse/write of the [phone] call-audio block. See header
 * for why parse and write live together (round-trip symmetry = anti-clobber).
 */

#include "tools/phone_audio_config.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

phone_ns_level_t phone_ns_level_from_string(const char *s) {
   if (s) {
      if (strcasecmp(s, "off") == 0) {
         return PHONE_NS_OFF;
      }
      if (strcasecmp(s, "low") == 0) {
         return PHONE_NS_LOW;
      }
      if (strcasecmp(s, "high") == 0) {
         return PHONE_NS_HIGH;
      }
      if (strcasecmp(s, "veryhigh") == 0) {
         return PHONE_NS_VERYHIGH;
      }
   }
   return PHONE_NS_MODERATE;
}

const char *phone_ns_level_to_string(phone_ns_level_t lvl) {
   switch (lvl) {
      case PHONE_NS_OFF:
         return "off";
      case PHONE_NS_LOW:
         return "low";
      case PHONE_NS_HIGH:
         return "high";
      case PHONE_NS_VERYHIGH:
         return "veryhigh";
      case PHONE_NS_MODERATE:
      default:
         return "moderate";
   }
}

phone_audio_config_t phone_audio_config_default(void) {
   phone_audio_config_t c;
   c.pcm_port[0] = '\0';
   c.uplink = phone_apm_default_config();
   /* Downlink derives from the uplink defaults with a few live-tuned overrides
    * (2026-07-09): NS off — the far-end audio is already NS/DTX-processed by the
    * cellular codec and a second pass is the classic "underwater" artifact; a
    * higher fixed gain than the uplink; echo_cancel N/A (no reverse stream on the
    * downlink).  Default ON — AGC normalizes quiet/loud callers a fixed gain can't. */
   c.downlink = phone_apm_default_config();
   c.downlink.fixed_gain_db = 6.0f;
   c.downlink.ns_level = PHONE_NS_OFF;
   c.downlink.echo_cancel = false;
   c.downlink_use_apm = true;
   c.downlink_gain = 0.0f; /* 0 -> bridge default (soft-limiter fallback, unused when APM on) */
   return c;
}

/* Parse the shared per-direction APM knobs "<prefix>_agc", "<prefix>_fixed_gain_db",
 * etc.  echo_cancel and the downlink use-flag are handled by the caller (they are
 * not symmetric across the two directions). */
static void parse_apm_block(toml_table_t *t, const char *prefix, phone_apm_config_t *out) {
   char key[48];
   toml_datum_t d;

   snprintf(key, sizeof(key), "%s_agc", prefix);
   d = toml_bool_in(t, key);
   if (d.ok) {
      out->agc_enabled = d.u.b;
   }
   snprintf(key, sizeof(key), "%s_fixed_gain_db", prefix);
   d = toml_double_in(t, key);
   if (d.ok) {
      out->fixed_gain_db = (float)d.u.d;
   }
   snprintf(key, sizeof(key), "%s_agc_ramp_db_per_s", prefix);
   d = toml_double_in(t, key);
   if (d.ok) {
      out->max_gain_change_db_per_s = (float)d.u.d;
   }
   snprintf(key, sizeof(key), "%s_max_output_noise_dbfs", prefix);
   d = toml_double_in(t, key);
   if (d.ok) {
      out->max_output_noise_dbfs = (float)d.u.d;
   }
   snprintf(key, sizeof(key), "%s_ns_level", prefix);
   d = toml_string_in(t, key);
   if (d.ok) {
      out->ns_level = phone_ns_level_from_string(d.u.s);
      free(d.u.s);
   }
   snprintf(key, sizeof(key), "%s_high_pass", prefix);
   d = toml_bool_in(t, key);
   if (d.ok) {
      out->high_pass = d.u.b;
   }
}

void phone_audio_config_parse(const toml_table_t *table, phone_audio_config_t *out) {
   if (!table || !out) {
      return;
   }
   /* toml accessors take a non-const table; the lib does not mutate it. */
   toml_table_t *t = (toml_table_t *)table;

   toml_datum_t port = toml_string_in(t, "pcm_port");
   if (port.ok) {
      snprintf(out->pcm_port, sizeof(out->pcm_port), "%s", port.u.s);
      free(port.u.s);
   }

   parse_apm_block(t, "uplink", &out->uplink);
   toml_datum_t ec = toml_bool_in(t, "uplink_echo_cancel"); /* uplink-only */
   if (ec.ok) {
      out->uplink.echo_cancel = ec.u.b;
   }

   parse_apm_block(t, "downlink", &out->downlink);
   toml_datum_t dapm = toml_bool_in(t, "downlink_apm");
   if (dapm.ok) {
      out->downlink_use_apm = dapm.u.b;
   }
   toml_datum_t dg = toml_double_in(t, "downlink_gain");
   if (dg.ok) {
      out->downlink_gain = (float)dg.u.d;
   }
}

/* Mirror of parse_apm_block — must emit exactly the keys it reads. */
static void write_apm_block(FILE *fp, const char *prefix, const phone_apm_config_t *c) {
   fprintf(fp, "%s_agc = %s\n", prefix, c->agc_enabled ? "true" : "false");
   fprintf(fp, "%s_fixed_gain_db = %g\n", prefix, (double)c->fixed_gain_db);
   fprintf(fp, "%s_agc_ramp_db_per_s = %g\n", prefix, (double)c->max_gain_change_db_per_s);
   fprintf(fp, "%s_max_output_noise_dbfs = %g\n", prefix, (double)c->max_output_noise_dbfs);
   fprintf(fp, "%s_ns_level = \"%s\"\n", prefix, phone_ns_level_to_string(c->ns_level));
   fprintf(fp, "%s_high_pass = %s\n", prefix, c->high_pass ? "true" : "false");
}

void phone_audio_config_write(FILE *fp, const phone_audio_config_t *cfg) {
   if (!fp || !cfg) {
      return;
   }
   /* pcm_port is a validated device node, but guard the unescaped TOML string
    * anyway (mirror ha_write_config). */
   if (cfg->pcm_port[0]) {
      bool safe = true;
      for (const char *p = cfg->pcm_port; *p; p++) {
         if (*p == '"' || *p == '\\' || *p == '\n' || *p == '\r') {
            safe = false;
            break;
         }
      }
      if (safe) {
         fprintf(fp, "pcm_port = \"%s\"\n", cfg->pcm_port);
      }
   }

   write_apm_block(fp, "uplink", &cfg->uplink);
   fprintf(fp, "uplink_echo_cancel = %s\n", cfg->uplink.echo_cancel ? "true" : "false");

   write_apm_block(fp, "downlink", &cfg->downlink);
   fprintf(fp, "downlink_apm = %s\n", cfg->downlink_use_apm ? "true" : "false");
   fprintf(fp, "downlink_gain = %g\n", (double)cfg->downlink_gain);
}
