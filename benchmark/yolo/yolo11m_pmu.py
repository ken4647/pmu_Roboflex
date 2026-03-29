from ultralytics import YOLO
from pmu_tick_api import robflex_set_as_latency_flick, robflex_shot_on_latency
import time

# Load a COCO-pretrained YOLO11m model
model = YOLO("benchmark/yolo/yolo11m.pt")
robflex_set_as_latency_flick(30_000_000, 20_000_000)
for i in range(100):
    t0 = time.time()
    results = model("benchmark/yolo/bus.jpg", verbose=False)
    # t1 = time.time()
    robflex_shot_on_latency()
    t2 = time.time()
    # print(f"Time taken: {t2 - t0} seconds, {t1 - t0} seconds")
    print(f"Time taken: {t2 - t0} seconds")