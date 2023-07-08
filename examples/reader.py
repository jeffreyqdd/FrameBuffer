#!/usr/bin/env python3

"""reads from the buffer created by webcam.py"""

import cv2
import time
from accessor import BufferedFrameReader

reader = BufferedFrameReader('webcam')

print('started video playback')
try:
    while True:
        frame, acq_time = reader.get_next_frame()
        now = int(time.time() * 1000)
        print(f'\rlatency ms: {now -acq_time}    ', end='')

        cv2.imshow('camera', frame)
        key = cv2.waitKey(1)
        if key == 27:  # exit on ESC
            break
except KeyboardInterrupt:
    pass

cv2.destroyAllWindows()
print('all cleaned up')
