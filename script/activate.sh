# Setup environment variables for pmu_roboniflex
# 
# Usage:
#  source env.bash
#  run your application here, e.g.:
#  ./my_application

# 使用脚本所在目录定位项目根，确保路径正确
_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
export LD_PRELOAD="${_SCRIPT_DIR}/../build/librobonix.so"
export ROBFLEX_ENABLE_FEATURES=1