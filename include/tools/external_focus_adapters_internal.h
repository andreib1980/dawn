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
 * @file external_focus_adapters_internal.h
 * @brief Internal-only constants shared across L3 external focus
 *        adapters (document, calendar, future email).
 *
 * Included only by the three external adapter source files
 * (`document_focus_adapter.c`, `calendar_focus_adapter.c`,
 * `external_focus_adapters_init.c`) — NOT a public API.  Per-adapter
 * constants live file-static inside their own .c; this header only
 * holds genuinely-cross-adapter values.
 */

#ifndef EXTERNAL_FOCUS_ADAPTERS_INTERNAL_H
#define EXTERNAL_FOCUS_ADAPTERS_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Multi-account cap applied per-adapter to bound the per-compose
 * fanout cost of users with many calendar / email accounts.  Three
 * suffices for nearly every real-world account set; users beyond the
 * cap log a one-time WARNING and the additional accounts are dropped
 * with deterministic ordering (db_account_list result order).
 *
 * TODO(1j): bench-driven tuning + config promotion.  Leaving
 * file-scope so 1j can land in one place across all external adapters. */
#define EXTERNAL_MAX_ACCOUNTS_PER_COMPOSE 3

#ifdef __cplusplus
}
#endif

#endif /* EXTERNAL_FOCUS_ADAPTERS_INTERNAL_H */
