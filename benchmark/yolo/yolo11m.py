from ultralytics import YOLO
import time

# Load a COCO-pretrained YOLO11m model
model = YOLO("benchmark/yolo/yolo11m.pt", verbose=False)

for i in range(100):
    t0 = time.time()
    results = model("benchmark/yolo/bus.jpg", verbose=False)
    t1 = time.time()
    print(f"Time taken: {t1 - t0} seconds")