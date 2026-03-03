mkdir -p build
cd build
cmake ..
cmake --build .

cd ../

sudo setcap cap_sys_nice+ep ./build/robonix_daemon

echo "Done"