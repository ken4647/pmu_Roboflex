/**
 * CUDA benchmark app for HPF-shmem evaluation.
 *
 * Timing (CSV column kernel_ms):
 *   Wall-clock ms for ONE kernel launch + cudaStreamSynchronize on that stream.
 *   Does NOT include the sleep between iterations.
 *
 * Workload: exactly ONE <<<>>> launch per sample; inner_repeat fuses work so one
 * launch targets ~ --kernel-ms (GPU-dependent; tune --inner-repeat if needed).
 */
 #include <chrono>
 #include <cmath>
 #include <thread>
 #include <cstdio>
 #include <cstdlib>
 #include <cstring>
 #include <cuda_runtime.h>
 
 #define CUDA_CHECK(call)                                                         \
     do {                                                                         \
         cudaError_t err = (call);                                                \
         if (err != cudaSuccess) {                                                \
             fprintf(stderr, "CUDA error %s:%d: %s\n", __FILE__, __LINE__,        \
                     cudaGetErrorString(err));                                    \
             exit(EXIT_FAILURE);                                                  \
         }                                                                        \
     } while (0)
 
 // One launch per measurement; inner_repeat scales GPU time per element.
 static int g_vector_size = 1 << 25;
 static int g_inner_repeat = 256;
 static int g_interval_ms = 50;
 static int g_iterations = 100;
 static int g_proc_id = 0;
 static bool g_high_priority = true;
 
 __global__ void vector_add_repeat(const float* A, const float* B, float* C, int n, int repeat)
 {
     int i = blockDim.x * blockIdx.x + threadIdx.x;
     if (i >= n) return;
     float a = A[i];
     float b = B[i];
     float c = a + b;
 #pragma unroll 1
     for (int r = 0; r < repeat; ++r) {
         c = fmaf(c, 1.000001f, b);
     }
     C[i] = c;
 }
 
 void parse_args(int argc, char** argv)
 {
     for (int i = 1; i < argc; ++i) {
         if (strcmp(argv[i], "--priority") == 0 && i + 1 < argc) {
             g_high_priority = (strcmp(argv[++i], "high") == 0);
         } else if (strcmp(argv[i], "--kernel-ms") == 0 && i + 1 < argc) {
             int target_ms = atoi(argv[++i]);
             // Heuristic: one grid pass ~0.3–2ms on 1<<25; scale inner_repeat ~ linearly.
             if (target_ms <= 1) {
                 g_vector_size = 1 << 22;
                 g_inner_repeat = 48;
             } else if (target_ms <= 5) {
                 g_vector_size = 1 << 24;
                 g_inner_repeat = 96;
             } else if (target_ms <= 20) {
                 g_vector_size = 1 << 25;
                 g_inner_repeat = 320;
             } else {
                 g_vector_size = 1 << 25;
                 g_inner_repeat = 320 * target_ms / 20;
             }
         } else if (strcmp(argv[i], "--inner-repeat") == 0 && i + 1 < argc) {
             g_inner_repeat = atoi(argv[++i]);
         } else if (strcmp(argv[i], "--interval-ms") == 0 && i + 1 < argc) {
             g_interval_ms = atoi(argv[++i]);
         } else if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
             g_iterations = atoi(argv[++i]);
         } else if (strcmp(argv[i], "--id") == 0 && i + 1 < argc) {
             g_proc_id = atoi(argv[++i]);
         } else if (strcmp(argv[i], "--vector-size") == 0 && i + 1 < argc) {
             g_vector_size = 1 << atoi(argv[++i]);
         }
     }
 }
 
 float* h_A = nullptr, * h_B = nullptr, * h_C = nullptr;
 float* d_A = nullptr, * d_B = nullptr, * d_C = nullptr;
 cudaStream_t stream;
 
 void prepare()
 {
     size_t size = (size_t)g_vector_size * sizeof(float);
     h_A = (float*)malloc(size);
     h_B = (float*)malloc(size);
     h_C = (float*)malloc(size);
     for (int i = 0; i < g_vector_size; ++i) {
         h_A[i] = static_cast<float>(rand()) / RAND_MAX;
         h_B[i] = static_cast<float>(rand()) / RAND_MAX;
     }
     CUDA_CHECK(cudaMalloc(&d_A, size));
     CUDA_CHECK(cudaMalloc(&d_B, size));
     CUDA_CHECK(cudaMalloc(&d_C, size));
     CUDA_CHECK(cudaMemcpy(d_A, h_A, size, cudaMemcpyHostToDevice));
     CUDA_CHECK(cudaMemcpy(d_B, h_B, size, cudaMemcpyHostToDevice));
     CUDA_CHECK(cudaStreamCreate(&stream));
 }
 
 /// Single kernel launch; GPU work ~= vector_size * inner_repeat (per thread).
 void run_kernel()
 {
     int block_size = 256;
     int grid_size = (g_vector_size + block_size - 1) / block_size;
     vector_add_repeat<<<grid_size, block_size, 0, stream>>>(
         d_A, d_B, d_C, g_vector_size, g_inner_repeat);
     CUDA_CHECK(cudaStreamSynchronize(stream));
     CUDA_CHECK(cudaGetLastError());
 }
 
 void cleanup()
 {
     if (d_A) CUDA_CHECK(cudaFree(d_A));
     if (d_B) CUDA_CHECK(cudaFree(d_B));
     if (d_C) CUDA_CHECK(cudaFree(d_C));
     free(h_A);
     free(h_B);
     free(h_C);
 }
 
 int main(int argc, char** argv)
 {
     parse_args(argc, argv);
     prepare();
 
     fprintf(stderr,
             "# proc_id=%d priority=%s interval_ms=%d (sleep between samples)\n"
             "# one_kernel_launch: vector 2^%d inner_repeat=%d (target ~--kernel-ms)\n"
             "# kernel_ms in CSV = wall ms for that ONE launch + cudaStreamSynchronize\n",
             g_proc_id, g_high_priority ? "high" : "low", g_interval_ms,
             (int)(0.5 + log2((double)g_vector_size)), g_inner_repeat);
 
     printf("proc_id,priority,task,kernel_ms,total_ms\n");
     fflush(stdout);  // stdout is fully buffered when redirected to a file
 
     for (int i = 0; i < g_iterations; ++i) {
         auto t0 = std::chrono::high_resolution_clock::now();
         run_kernel();
         auto t1 = std::chrono::high_resolution_clock::now();
         auto kernel_ms = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0;
         auto total_ms = kernel_ms;
         printf("%d,%s,%d,%.2f,%.2f\n", g_proc_id, g_high_priority ? "high" : "low", i, kernel_ms, total_ms);
         fflush(stdout);
 
         if (i < g_iterations - 1 && g_interval_ms - (int)kernel_ms > 0) {
             std::this_thread::sleep_for(std::chrono::milliseconds((int)(g_interval_ms - (int)kernel_ms)));
         }
     }
 
     cleanup();
     return 0;
 }
 