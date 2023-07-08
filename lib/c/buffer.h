#pragma once

#ifdef __cplusplus
extern "C" {
#endif
/* Buffer.h provides **safe** utility functions to read/write frames/frame-like data
 * to a mmap-backed buffer
 *
 * To maintain clarity, I will refer to **image** as the "raw" rgb data and **frame** as
 * the image + metadata.
 *
 * The system consists of 3 main structures
 *      1. block_t: A block is the high-level encapsulation. Think of a block
 *          as the main access point to all the frame data and mutexes. Blocks
 *          are non-unique since they only serve as an access point and every
 *          read/write process must have a block structure.
 *
 *      2. buffer_t: Refers to the frame buffer. Contains raw image data and
 *          metadata. It maintains a “master” mutex that all read/write processes
 *          need to interact with. For each frame, it also maintains a frame mutex
 *          which is a pthread rwlock t. This way, the resource allows for multiple
 *          readers at the same time when the data is not being written to.
 *          Buffers are unique. There can only be one buffer of the same name
 *          located at /dev/shm/. Buffers have only 1 owner. Only the owner has
 *          write access. All other processes only have read capabilities. This is
 *          enforced using PIDs.

 *          The buffer is located on /dev/shm/ because that directory is a TPFS
 *          mounted on RAM. This allows for lightning-fast read/write operations
 *          without degradation to the NAND flash memory storage modules on
 *          modern-day solid state storage.
 *
 *      3. frame_t: represents a single frame. Contains image dimensions, acquisition time,
 *          and raw image data. (exposed to client)
 *
 * learn about mutexes here
 *      - http://www.cs.kent.edu/~ruttan/sysprog/lectures/multi-thread/pthread_cond_init.html
 *      - https://docs.oracle.com/cd/E19455-01/806-5257/6je9h032u/index.html
 *
 */

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

// The number of images held in a buffer.
#define BUFFER_COUNT 3

// where to store the buffer
#define BLOCK_DIR "/dev/shm/buffer-"

// A frame is a long array of characters. Each character is 1 byte, which perfectly represents
// a pixel value [0,255].
typedef unsigned char image;

typedef struct buffer buffer_t;
typedef struct block block_t;

typedef struct frame {
    size_t width, height, depth;
    uint64_t acquisition_time;
    uint64_t frame_uid;
    image* data;
} frame_t;

/* ############################################################################
 * The following section deals with block_t management. One may create or destroy
 * blocks, register readers for existing blocks, and recover poisoned blocks.
 *
 * Important note 1: if the *owner* of the block crashes (ungracefully), the
 * block is now known as *poisoned* if a reader of the block crashes, the block
 * remains healthy. Poisoned blocks must be deleted and recreated!
 * ############################################################################
 */

// Allocates a new [block_t] struct with name [direction] backed by a mmap [buffer_t]
// located at [BLOCK_DIR]-[direction]. Memory is allocated for this buffer and the
// caller of this function will be set as the owner`. The buffer will hold
// [BUFFER_COUNT] images, each of size [width] * [height] * [depth] bytes.
//
// Preconditions:
//  - [direction] cannot have a "/" character and there must not be an existing block
//      located at [BLOCK_DIR]-[direction]
//
//  usage:
//      block_t* forward = create_block("forward");
//      <...omitted...>
//      destroy_block(forward):
block_t* create_block(const char* direction, size_t width, size_t height, size_t depth);

// Allocates a new [block_t] struct with name [direction] that points to an already exsting
// mmap backed buffer_t located at [BLOCK_DIR]-[direciton]. This function is non-blocking.
//
// Preconditions:
//  - [direction] cannot have a "/" character. If violated, an error message is
//      printed to stderr, and the NULL value is returned
//  - the block must exist. If not, the NULL value is returned.
//
//  usage:
//      block_t* forward = access_block("forward");
//      <...omitted...>
//      close_block(forward):
block_t* open_block(const char* direction);

// Returns the size in bytes required to hold a singular image in block_t [b]
size_t block_image_size(const block_t* b);

// Returns true if the block whose buffer is backed at [BLOCK_DIR]-[direction]
// is poisoned.
//
// WARNING: if the process is the owner of the buffer backed at [block],
// then do not use the cstr variant,
bool cstr_block_is_poisoned(const char* direction);
bool block_is_poisoned(const block_t* block);

// Returns true if the block whose buffer is backed at [BLOCK_DIR]-[direction]
// has an active writer
//
// WARNING: if the process is the owner of the buffer backed at [block],
// then do not use the cstr variant,
bool cstr_block_is_alive(const char* direction);
bool block_is_alive(const block_t* block);

// Attempts to free memory associated withc[block] and destroy the buffer that
// is pointed to by [block]. This function is blocking and will terminate when
//      1) all read processes are stopped
//      2) when all the data associated with [block] and the underlying [buffer]
//          have been safely unallocated.
// If the current process is not the owner of the [block], and the block is poisoned,
// then this function acts as if the current process is the owner. If the current
// process is not the owner of the [block], and the block is NOT poisoned, then
// this function will do nothing.
void destroy_block(block_t* block);

// Frees memory associated with [block]. DOES NOT FREE UNDERLYING BUFFER.
// Requires that [block] does not own the buffer (destroy block should be used
// instead).
//
// Preconditions:
//  - [block] does not own underlying buffer
void close_block(block_t* block);

/* ############################################################################
 * The following section deals with reading and writing from blocks.
 * Note: care should be taken to handle for each failure case to avoid
 *  undesired behavior
 * ############################################################################
 */

// exit codes returned from block access operations
//  - SUCCESS: operation terminated as expected
//  - FRAME_SIZE_MISMATCH: frame dimension is not the same as the block's
//      image buffer dimension
//  - BLOCK_NOT_ACTIVE: there is no owner of the block and thus the data is stale
//  - NO_NEW_FRAME: there are no new frames in the buffer
#define SUCCESS 0
#define FRAME_SIZE_MISMATCH 1
#define BLOCK_NOT_ACTIVE 2
#define NO_NEW_FRAME 3

// Writes the image data in [frame] to [buffer]
int write_frame(block_t* block, size_t width, size_t height,
                size_t depth, uint64_t acquisition_time, image* data);

// Reads the earliest frame in [buffer] that is newer than the image held in [frame].
// [read_frame] will wait for a new frame if [block_thread] is true. Else,
// it terminates with exit code [NO_NEW_FRAME], and [frame] is unchanged.
int read_frame(block_t* block, frame_t* frame, bool block_thread);

// Creates an empty frame struct in a way such that all images are newer than
// than the image held in the struct and returns a pointer to it.
frame_t* create_frame();

// given [ptr] for frame_t, this function safely frees the memory
// and deletes [ptr]
void delete_frame(frame_t* ptr);

#ifdef __cplusplus
}
#endif