export ROBFLEX_ENABLE_FEATURES=0
unset LD_PRELOAD
unset ROBFLEX_CYCLES_NUM_IN_MILLION

mkdir -p build
cd build
cmake ..
cmake --build .

cd ../

sudo setcap cap_sys_nice+ep ./build/robonix_daemon

echo "Done"