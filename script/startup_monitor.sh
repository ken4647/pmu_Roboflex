# !/bin/bash
_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"

source ${_SCRIPT_DIR}/deactivate.sh

${_SCRIPT_DIR}/../build/proc_stat