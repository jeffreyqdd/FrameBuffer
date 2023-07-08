#!/usr/bin/env python3

"""creates a buffer named webcam"""

import cv2
import time
from accessor import BufferedFrameWriter

writer = BufferedFrameWriter('webcam')

vc = cv2.VideoCapture(0)

try:
    print('started video capture')
    while True:
        _, frame = vc.read()
        writer.write_frame(frame, int(time.time() * 1000))
except KeyboardInterrupt:
    pass

vc.release()
cv2.destroyAllWindows()
print('all cleaned up')
