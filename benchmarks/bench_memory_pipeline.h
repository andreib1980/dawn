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
 * Memory-pipeline mode public surface for bench_retrieval.
 */

#ifndef BENCH_MEMORY_PIPELINE_H
#define BENCH_MEMORY_PIPELINE_H

/**
 * Initialize :memory: SQLite + apply BENCH_MEMORY_DDL.
 * Inserts a benchmark user with id=1.  Must be called instead of (not
 * alongside) the existing setup_db() — the two DDLs use incompatible
 * users-table schemas.
 *
 * Returns 0 on success, 1 on failure.
 */
int bench_mp_init(void);

/**
 * Close the in-memory database.
 */
void bench_mp_teardown(void);

/**
 * Run a smoke-test pass: ingest one LoCoMo conversation, fire extraction
 * at each session boundary, dump facts + provenance to stdout as JSON.
 *
 * @param locomo_path  Path to LoCoMo dataset (e.g., locomo10.json)
 * @param conv_idx     Index of the conversation to process (0-9)
 * @return 0 on success, 1 on failure.
 */
int bench_mp_run_smoke(const char *locomo_path, int conv_idx);

#endif /* BENCH_MEMORY_PIPELINE_H */
