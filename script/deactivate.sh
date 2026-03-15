# Setup environment variables for pmu_roboniflex
# 
# Usage:
#  source env.bash
#  run your application here, e.g.:
#  ./my_application

# export LD_PRELOAD=$(pwd)/build/librobonix.so
export ROBFLEX_ENABLE_FEATURES=0
unset LD_PRELOAD
unset ROBFLEX_CYCLES_NUM_IN_MILLION