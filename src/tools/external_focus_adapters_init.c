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
 * External focus-source adapter registration entrypoint — Phase 1d.
 *
 * Each L3 adapter (document, calendar) defines its own file-static
 * `k_<name>_focus_adapter` and exposes a `<name>_focus_adapter_register`
 * thunk that calls `focus_register_source()`.  This file's
 * `external_focus_adapters_register_all()` invokes them in a fixed
 * order; the order is stable for the framework's iteration and for
 * tests that assert per-adapter behavior.
 *
 * Email adapter NOT registered — DEFERRED to v2.  The email subsystem
 * has no message-level cache (only `email_accounts`); a DB-direct
 * adapter is impossible without first building an email message
 * cache.  See `external_focus_adapters.h` and the design doc's
 * Phase 1d API audit for the rationale.
 */

#include "dawn_error.h"
#include "logging.h"
#include "tools/external_focus_adapters.h"

/* Per-adapter register thunks — declared here rather than via header
 * to keep the per-adapter `.c` files free of cross-file plumbing. */
int document_focus_adapter_register(void);
int calendar_focus_adapter_register(void);

int external_focus_adapters_register_all(void) {
   if (document_focus_adapter_register() != SUCCESS) {
      OLOG_ERROR("external_focus_adapters: document adapter registration failed");
      return FAILURE;
   }
   if (calendar_focus_adapter_register() != SUCCESS) {
      OLOG_ERROR("external_focus_adapters: calendar adapter registration failed");
      return FAILURE;
   }
   /* email adapter — deferred to v2 (needs cache layer prerequisite) */
   return SUCCESS;
}
