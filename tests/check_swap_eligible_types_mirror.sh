#!/bin/bash
#
# Verifies that the entity-type list for the longer-canonical direction
# preference stays in sync between:
#
#   C  : src/memory/memory_db_alias_writes.c
#        function entity_type_prefers_longer_canonical
#   JS : www/js/ui/memory_aliases.js
#        constant SWAP_ELIGIBLE_TYPES
#
# Two sources of truth for the same predicate (the WebUI manual-link
# direction-preference suggestion can't share C code), so the architecture-
# reviewer flagged this as a drift risk on the Path B + Path C plan.  Mirror
# banners on both sides point at each other; this test backstops the banner
# by failing CI if the literal type lists diverge.
#
# Wired into ctest -L ci via tests/CMakeLists.txt as
# `check_swap_eligible_types_mirror`.
#

set -e
set -u
# pipefail propagates failures from awk/grep inside extraction pipes —
# without it, set -e only catches the LAST command in a pipeline and
# awk parse errors would silently produce empty output that gets
# downstream-caught only by the empty-string guard.
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

C_FILE="${REPO_ROOT}/src/memory/memory_db_alias_writes.c"
JS_FILE="${REPO_ROOT}/www/js/ui/memory_aliases.js"

if [ ! -f "${C_FILE}" ]; then
   echo "ERROR: C source not found: ${C_FILE}" >&2
   exit 2
fi
if [ ! -f "${JS_FILE}" ]; then
   echo "ERROR: JS source not found: ${JS_FILE}" >&2
   exit 2
fi

# Extract the C type list from the function body.  awk scans from the
# function declaration to the closing brace, then grep pulls out each
# double-quoted token inside the strcmp() chain.
C_TYPES=$(awk '
   /entity_type_prefers_longer_canonical/ { in_func = 1 }
   in_func { print }
   in_func && /^}/ { exit }
' "${C_FILE}" | grep -oE '"[a-z]+"' | tr -d '"' | sort -u)

# Extract the JS type list from the Set literal.  awk scans from
# SWAP_ELIGIBLE_TYPES to the next closing brace; grep pulls each
# single-quoted token from the Set initializer.
JS_TYPES=$(awk "
   /SWAP_ELIGIBLE_TYPES/ { in_set = 1 }
   in_set { print }
   in_set && /\)/ { in_set = 0 }
" "${JS_FILE}" | grep -oE "'[a-z]+'" | tr -d "'" | sort -u)

if [ -z "${C_TYPES}" ]; then
   echo "ERROR: failed to extract C-side type list (regex broken?)" >&2
   echo "Source: ${C_FILE}" >&2
   exit 2
fi
if [ -z "${JS_TYPES}" ]; then
   echo "ERROR: failed to extract JS-side type list (regex broken?)" >&2
   echo "Source: ${JS_FILE}" >&2
   exit 2
fi

if [ "${C_TYPES}" != "${JS_TYPES}" ]; then
   echo "ERROR: swap-eligible entity-type list drift between C and JS!" >&2
   echo "" >&2
   echo "C  (${C_FILE}::entity_type_prefers_longer_canonical):" >&2
   echo "${C_TYPES}" | sed 's/^/   /' >&2
   echo "" >&2
   echo "JS (${JS_FILE}::SWAP_ELIGIBLE_TYPES):" >&2
   echo "${JS_TYPES}" | sed 's/^/   /' >&2
   echo "" >&2
   echo "Update the mirror banner comments AND both lists when changing" >&2
   echo "the swap-eligible types.  See the banners in each file for the" >&2
   echo "rationale and architecture-reviewer finding." >&2
   exit 1
fi

echo "OK: swap-eligible types match between C and JS (${C_TYPES})"
exit 0
