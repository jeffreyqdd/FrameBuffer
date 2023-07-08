import numpy as np
import time
from ctypes import (
    POINTER,
    c_ubyte,
    c_uint64,
    c_char_p,
    c_void_p,
    c_ssize_t,
    c_bool,
    c_int32,
    Structure,
    addressof,
    cdll
)

# return values of _lib.read_frame and _lib.write_frame
SUCCESS = 0
FRAME_SIZE_MISMATCH = 1
BLOCK_NOT_ACTIVE = 2
NO_NEW_FRAME = 3


class _Frame(Structure):
    _fields_ = [
        ("width", c_ssize_t),
        ("height", c_ssize_t),
        ("depth", c_ssize_t),
        ("acquisition_time", c_uint64),
        ("frame_uid", c_uint64),
        ("data", POINTER(c_ubyte)),
    ]


_lib = cdll.LoadLibrary('libbuffer.so')

# block_t* create_block(const char* direction, size_t width, size_t height, size_t depth);
_lib.create_block.argtypes = (
    c_char_p, c_ssize_t, c_ssize_t, c_ssize_t)
_lib.create_block.restype = c_void_p

# block_t* open_block(const char* direction);
_lib.open_block.argtypes = (c_char_p,)
_lib.open_block.restype = c_void_p

# bool cstr_block_is_poisoned(const char* direction);
_lib.cstr_block_is_poisoned.argtypes = (c_char_p,)
_lib.cstr_block_is_poisoned.restype = c_bool
# bool block_is_poisoned(const block_t* block);
_lib.block_is_poisoned.argtypes = (c_void_p,)
_lib.block_is_poisoned.restype = c_bool

# bool cstr_block_is_alive(const char* direction);
_lib.cstr_block_is_alive.argtypes = (c_void_p,)
_lib.cstr_block_is_alive.restype = c_bool
# bool block_is_alive(const block_t* block);
_lib.block_is_alive.argtypes = (c_void_p,)
_lib.block_is_alive.restype = c_bool

# void destroy_block(block_t* block);
_lib.destroy_block.argtypes = (c_void_p,)
_lib.destroy_block.restype = None


# void close_block(block_t* block);
_lib.close_block.argtypes = (c_void_p,)
_lib.close_block.restype = None

# int write_frame(block_t* block, size_t width, size_t height,
#                 size_t depth, uint64_t acquisition_time, image* data);
_lib.write_frame.argtypes = (
    c_void_p,
    c_ssize_t,
    c_ssize_t,
    c_ssize_t,
    c_uint64,
    c_void_p,
)
_lib.write_frame.restype = c_int32

# int read_frame(block_t* block, frame_t* frame);
_lib.read_frame.argtypes = (c_void_p, c_void_p, c_bool)
_lib.read_frame.restype = c_int32


# frame_t* create_frame();
_lib.create_frame.argtypes = None
_lib.create_frame.restype = c_void_p

# void delete_frame(frame_t* ptr);
_lib.delete_frame.argtypes = c_void_p,
_lib.delete_frame.restype = None

# size_t block_image_size(const block_t* b);
_lib.block_image_size.argtypes = c_void_p,
_lib.block_image_size.restype = c_ssize_t


class ExistentialError(Exception):
    pass


class NoFrameError(Exception):
    pass


class BufferedFrameWriter:
    def __init__(self, name: str):
        self.name = name
        self._block = None

    def write_frame(self, frame: np.ndarray, acq_time: np.uint64):
        width = height = depth = 1
        if len(frame.shape) == 1:
            width = frame.shape[0]
        elif len(frame.shape) == 2:
            height, width = frame.shape
        else:
            height, width, depth = frame.shape

        if self._block is None:
            c_name = self.name.encode("utf-8")
            self._block = _lib.create_block(
                c_name, width, height, depth)
            if self._block is None and _lib.cstr_block_is_poisoned(c_name):
                tmp_block = _lib.open_block(c_name)
                _lib.destroy_block(tmp_block)
                time.sleep(1)
                self._block = _lib.create_block(
                    c_name, width, height, depth)
            if self._block is None:
                raise ExistentialError()

        exit_code = _lib.write_frame(
            self._block, width, height, depth, acq_time, frame.ctypes.data)

        if exit_code == FRAME_SIZE_MISMATCH:
            print(
                "Error: frame size mismatch. Please ensure input frames are consistent.")
        elif exit_code == BLOCK_NOT_ACTIVE:
            print("Block is not active.")

    def __del__(self):
        if self._block != None:
            _lib.destroy_block(self._block)


class BufferedFrameReader:
    def __init__(self, name: str):
        self.name = name
        self._frame = self._setup_accessor_frame()
        self._last_python_frame = None
        self._array = None
        self._block = None
        self._attach_to_block()

    def _attach_to_block(self, show_found_msg=False):
        while self._block is None:
            self._block = _lib.open_block(self.name.encode("utf-8"))
            if self._block is None:
                print(f"Block {self.name} dne. Waiting and trying again.")
                show_found_msg = True
                time.sleep(3)
        if show_found_msg:
            print(f"Found {self.name}!!!")

    def _setup_accessor_frame(self):
        frame = _Frame()
        frame.frame_uid = np.uint64(0)
        return frame

    def get_next_frame(self, wait_for_frame=True):
        curr_frame = self._frame

        exit_code = _lib.read_frame(
            self._block, addressof(curr_frame), wait_for_frame)

        if exit_code == BLOCK_NOT_ACTIVE:
            print(f"Lost access to {self.name}. Retrying open.")
            self._block = None
            self._attach_to_block(True)
            return self.get_next_frame()
        elif exit_code == FRAME_SIZE_MISMATCH:
            pass  # never be here.
        elif exit_code == NO_NEW_FRAME:
            return None
        shape = (curr_frame.height, curr_frame.width, curr_frame.depth)

        if (self._array is None or
                self._array.__array_interface__['data'][0] != addressof(curr_frame.data.contents)):
            self._array = np.ctypeslib.as_array(curr_frame.data, shape)

        self._last_python_frame = self._array, curr_frame.acquisition_time
        return self._last_python_frame

    def has_last_frame(self):
        return self._last_python_frame is not None

    def get_last_frame(self):
        if self._last_python_frame is None:
            raise NoFrameError()
        return self._last_python_frame

    def __del__(self):
        if self._block != None:
            _lib.close_block(self._block)
