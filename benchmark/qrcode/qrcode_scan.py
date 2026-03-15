import cv2
import time

# 读取图片 (请确保路径正确)
img = cv2.imread('benchmark/qrcode/helloworld_qrcode.png')

if img is None:
    print("错误：无法加载图片，请检查文件路径 'benchmark/qrcode/helloworld_qcode.png' 是否存在。")
    exit()

detector = cv2.QRCodeDetector()

print("开始性能测试 (按 Ctrl+C 停止)...")

try:
    while True:
        # 1. 记录开始时间 (秒)
        t0 = time.time()
        
        # 2. 执行识别
        data, bbox, straight_qrcode = detector.detectAndDecode(img)

        # 3. 记录结束时间
        t1 = time.time()
        
        # 4. 计算耗时 (转换为毫秒 ms)
        time_consumption = (t1 - t0) * 1000

        # 5. 输出结果
        if data:
            print(f"✅ 识别内容: {data}")
            print(f"📍 位置坐标: {bbox}")
        else:
            print("❌ 未检测到二维码")
        
        # 打印耗时，保留 2 位小数
        print(f"⏱️ 耗时: {time_consumption:.2f} ms")
        print("-" * 30)

        # 可选：稍微延时，避免终端输出刷屏太快看不清
        time.sleep(0.1) 

except KeyboardInterrupt:
    print("\n测试已停止。")