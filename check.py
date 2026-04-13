from ultralytics import YOLO
m=YOLO('yolo11n.pt')
print(m.model.model[-1])
print(getattr(m.model.model[-1], 'cv2', None))
