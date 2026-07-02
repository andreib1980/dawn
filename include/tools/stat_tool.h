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
 * STAT system-telemetry tool — LLM-facing descriptor ("system_status") that
 * reports live temps/battery/load/fan and historical trends from the STAT
 * sensor service.  Registered in src/tools/tools_init.c.
 */

#ifndef STAT_TOOL_H
#define STAT_TOOL_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Register the system_status tool with the tool registry. */
int stat_tool_register(void);

#ifdef __cplusplus
}
#endif

#endif /* STAT_TOOL_H */
