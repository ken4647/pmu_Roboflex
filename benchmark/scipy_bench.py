import scipy
import numpy as np
import time

grid_size = 4096

def main():
    print(f"SciPy version: {scipy.__version__}")
    _grid = np.random.random((grid_size, grid_size)).astype(np.float32)
    _output = np.zeros_like(_grid, dtype=np.float64)
    for i in range(5):
        print(f"Running distance transform iteration {i+1}/10...")
        start_time = time.time()
        scipy.ndimage.distance_transform_edt(_grid, return_distances=True, return_indices=False, distances=_output)
        end_time = time.time()
        print(f"Iteration {i+1} took {end_time - start_time:.4f} seconds")
    

if __name__ == "__main__":
    main()