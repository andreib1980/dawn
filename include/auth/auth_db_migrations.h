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
 * Authentication Database Migration Ladder — internal cross-module entry point.
 */

#ifndef AUTH_DB_MIGRATIONS_H
#define AUTH_DB_MIGRATIONS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Apply the per-version migration ladder on top of SCHEMA_SQL.
 *
 * Runs every migration step whose version window includes the DB's current
 * version, then bumps schema_version to AUTH_DB_SCHEMA_VERSION iff all gating
 * steps succeeded.  Defined in auth_db_migrations.c; called only by
 * auth_db_create_schema() in auth_db_schema.c.
 *
 * @param current_version  Schema version read before (re)applying SCHEMA_SQL (0 = fresh).
 * @param db_path          Path to the auth DB file (used to derive sibling dirs, e.g. images).
 * @return AUTH_DB_SUCCESS on success, AUTH_DB_FAILURE on a fatal migration error.
 */
int auth_db_apply_migrations(int current_version, const char *db_path);

#ifdef __cplusplus
}
#endif

#endif /* AUTH_DB_MIGRATIONS_H */
