_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"

cd $_SCRIPT_DIR/../third_party/xsched/output/bin

unset XSCHED_HPF_SHM
export XSCHED_HPF_SHM=ON

./xserver HPF 50000