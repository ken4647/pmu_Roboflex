from ultralytics import YOLO

# Load a COCO-pretrained YOLO26n model
model = YOLO("benchmark/yolo/yolo26n.pt")

for i in range(100):
    results = model("benchmark/yolo/bus.jpg", device="cpu")