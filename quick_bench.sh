# 前提条件
sudo apt update
sudo apt install -y build-essential cmake git

# build
bash script/build.sh

# run quick bench
python3 scipy_bench.py

