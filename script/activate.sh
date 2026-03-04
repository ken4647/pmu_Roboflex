# Setup environment variables for pmu_roboniflex
# 
# Usage:
#  source env.bash
#  run your application here, e.g.:
#  ./my_application

export LD_PRELOAD=$(pwd)/build/librobonix.so
export ROBFLEX_ENABLE_FEATURES=1
export ROBFLEX_INSTRUCTION_SLICE_1M=20