NVCC = nvcc
NVCC_FLAGS = -O3 -std=c++11
TARGET = cuda_bench_app

.PHONY: all clean

all: $(TARGET)

$(TARGET): cuda_bench_app.cu
	$(NVCC) $(NVCC_FLAGS) -o $@ $<

clean:
	rm -f $(TARGET)
