from ultralytics import YOLO

# Load a COCO-pretrained YOLO11m model
model = YOLO("benchmark/yolo/yolo11m.pt")

for i in range(100):
    results = model("benchmark/yolo/bus.jpg", device="cpu" )