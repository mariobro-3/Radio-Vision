import cv2

print("Scanning for available USB cameras...\n")

available_cameras = []

# Check camera indices 0-10
for i in range(10):
    cap = cv2.VideoCapture(i)
    if cap.isOpened():
        ret, frame = cap.read()
        if ret:
            # Get camera info
            width = cap.get(cv2.CAP_PROP_FRAME_WIDTH)
            height = cap.get(cv2.CAP_PROP_FRAME_HEIGHT)
            fps = cap.get(cv2.CAP_PROP_FPS)
            
            print(f"Camera Index {i}:")
            print(f"  Resolution: {int(width)}x{int(height)}")
            print(f"  FPS: {fps}")
            print(f"  Backend: {cap.getBackendName()}")
            print()
            
            available_cameras.append(i)
        cap.release()

if available_cameras:
    print(f"\nFound {len(available_cameras)} camera(s) at index(es): {available_cameras}")
    print(f"\nTo use a different camera, edit camera_streaming.py and change:")
    print(f"USB_CAMERA_INDEX = 0")
    print(f"to:")
    print(f"USB_CAMERA_INDEX = <your desired index>")
else:
    print("No cameras found!")

