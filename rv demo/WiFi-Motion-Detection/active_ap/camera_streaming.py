import sys
import time
import socket
import collections
import numpy as np
import pyqtgraph as pg
from pyqtgraph.Qt import QtCore, QtWidgets

import cv2

# USB Camera configuration
USB_CAMERA_INDEX = 1  # Changed to camera index 1
IMAGE_FRESH_INTERVAL = 20 # ms

class App(QtWidgets.QMainWindow):
    def __init__(self, parent=None):
        super(App, self).__init__(parent)

        #### Create Gui Elements ###########
        self.mainbox = pg.LayoutWidget()
        self.setCentralWidget(self.mainbox)

        # set up image widget
        self.img_w = pg.GraphicsLayoutWidget()
        self.mainbox.addWidget(self.img_w, row=0, col=0)
        # image view box
        self.view = self.img_w.addViewBox()
        self.view.setAspectLocked(True)
        self.view.setRange(QtCore.QRectF(0,0, 800, 600))
        #  image plot
        self.img = pg.ImageItem(border='w')
        self.view.addItem(self.img)

        # a text label widget for info dispaly
        self.label = QtWidgets.QLabel()
        self.mainbox.addWidget(self.label, row=1, col=0)

        #### Set Data  #####################

        self.fps = 0.
        self.lastupdate = time.time()

        # bring up camera and stream video
        self.setup_camera()

    def calculate_fps(self):
        now = time.time()
        dt = (now-self.lastupdate)
        if dt <= 0:
            dt = 0.000000000001
        fps2 = 1.0 / dt
        self.lastupdate = now
        self.fps = self.fps * 0.9 + fps2 * 0.1

    def update_label(self):
        tx = 'Mean Frame Rate:  {fps:.3f} FPS'.format(fps=self.fps )
        self.label.setText(tx)

    def setup_camera(self):
        # Open USB camera
        self.cap = cv2.VideoCapture(USB_CAMERA_INDEX)
        
        if not self.cap.isOpened():
            print(f"ERROR: Could not open USB camera at index {USB_CAMERA_INDEX}")
            print("Available camera indices to try: 0, 1, 2...")
            sys.exit(1)
        
        # Set camera resolution (optional)
        self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
        self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
        
        print(f"USB Camera opened successfully at index {USB_CAMERA_INDEX}")
        
        # schedule the first update call
        QtCore.QTimer.singleShot(1, self.update_img)

    def update_img(self):
        ret, frame = self.cap.read()
        
        if ret:
            # Convert BGR (OpenCV format) to RGB for display
            img_np = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
            
            # Rotate if needed (remove or adjust angle as needed)
            # img_np = np.rot90(img_np, k=-1)  # -1 = 90° clockwise
            
            self.img.setImage(img_np)
            self.calculate_fps()
            self.update_label()
        else:
            print("WARNING: Failed to read frame from USB camera")

        # schedule the next update call
        QtCore.QTimer.singleShot(IMAGE_FRESH_INTERVAL, self.update_img)
    
    def closeEvent(self, event):
        # Release the camera when closing
        if hasattr(self, 'cap'):
            self.cap.release()
        event.accept()


if __name__ == '__main__':

    app = QtWidgets.QApplication(sys.argv)
    thisapp = App()
    thisapp.show()
    if hasattr(app, "exec"):
        sys.exit(app.exec())
    else:
        sys.exit(app.exec_())