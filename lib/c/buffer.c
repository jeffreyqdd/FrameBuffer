// Look at buffer.h for in depth documentation
// Access buffers backed by mmap
#include "buffer.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define max(a, b) \
    ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })

typedef struct frame_metadata {
    uint64_t frame_uid;
    uint64_t acquisition_time;
    pthread_rwlock_t rwlock;
} frame_metadata_t;

typedef struct buffer {
    uint64_t frame_cnt;  // ideally our modules never run long enough to overflow this count
    size_t width, height, depth;
    bool is_alive;
    pid_t owner;
    pthread_cond_t cond;
    pthread_mutex_t cond_mutex;
    frame_metadata_t metadata[BUFFER_COUNT];
    image images[];
} buffer_t;

typedef struct block {
    char* filename;
    buffer_t* buffer;
} block_t;

// helper functions to grab size in bytes for a particular data structure
size_t buffer_image_size(const buffer_t* b) {
    return b->width * b->height * b->depth;
}
size_t frame_image_size(const frame_t* f) {
    return f->width * f->height * f->depth;
}
size_t buffer_size(size_t weight, size_t height, size_t depth) {
    return sizeof(buffer_t) + (weight * height * depth * BUFFER_COUNT);
}
size_t block_image_size(const block_t* b) {
    return buffer_image_size(b->buffer);
}

int write_frame(block_t* block, size_t width, size_t height, size_t depth,
                uint64_t acquisition_time, image* data) {
    buffer_t* buffer = block->buffer;

    // assert precondition: frame size is homogenous
    if (buffer->width != width ||
        buffer->height != height ||
        buffer->depth != depth) {
        return FRAME_SIZE_MISMATCH;
    }

    // assert precondition: block is active
    if (!buffer->is_alive) return BLOCK_NOT_ACTIVE;

    uint32_t buffer_to_write_to = (buffer->frame_cnt + 1) % BUFFER_COUNT;
    frame_metadata_t* metadata = &buffer->metadata[buffer_to_write_to];

    // grab write lock
    pthread_rwlock_wrlock(&metadata->rwlock);

    // write the image and corresponding metadata
    size_t image_size = buffer_image_size(buffer);
    memcpy(&buffer->images[image_size * buffer_to_write_to],
           data, image_size * sizeof(unsigned char));
    buffer->frame_cnt += 1;
    metadata->acquisition_time = acquisition_time;
    metadata->frame_uid = buffer->frame_cnt;

    // release write lock
    pthread_rwlock_unlock(&metadata->rwlock);

    // notify all watchers that a new image has been posted
    // we take care to avoid the following scenario: read thread sees that
    //      no frame is available -> broadcast -> read thread sleeps -> misses out on frame
    pthread_mutex_lock(&buffer->cond_mutex);
    pthread_cond_broadcast(&buffer->cond);
    pthread_mutex_unlock(&buffer->cond_mutex);

    return SUCCESS;
}

int read_frame(block_t* block, frame_t* frame, bool block_thread) {
    // this needs to be inside because of the case:
    // read sees buffer is alive -> buffer is not alive -> broadcast give up->
    // read thread sleeps -> forever asleep.

    pthread_mutex_lock(&block->buffer->cond_mutex);
    // figure out which frame needs to be grabbed
    buffer_t* buffer = block->buffer;

    // realloc frame data outside of locking any threads so code does not
    // block for longer than it needs to
    frame->data = (unsigned char*)realloc(frame->data,
                                          buffer_image_size(buffer));
    frame->width = buffer->width;
    frame->height = buffer->height;
    frame->depth = buffer->depth;

    // assert preconditions
    if (!buffer->is_alive) {
        pthread_mutex_unlock(&buffer->cond_mutex);
        return BLOCK_NOT_ACTIVE;
    }

    uint64_t newest_buffer = buffer->frame_cnt;
    uint64_t last_buffer = frame->frame_uid;

    uint64_t target_frame_uid;
    if (newest_buffer < BUFFER_COUNT)
        target_frame_uid = last_buffer + 1;
    else
        target_frame_uid = max(
            last_buffer + 1,
            newest_buffer - BUFFER_COUNT + 1);

    // this is within the range of an integer.
    int target_buffer = target_frame_uid % BUFFER_COUNT;

    // try to accquire read lock, if it isn't available,
    // register self as watcher
    frame_metadata_t* metadata = &(buffer->metadata[target_buffer]);

    if (last_buffer == newest_buffer) {
        if (block_thread) {
            // if the newest frame is not yet available
            pthread_cond_wait(&buffer->cond, &buffer->cond_mutex);
            if (!buffer->is_alive) {
                pthread_mutex_unlock(&buffer->cond_mutex);
                return BLOCK_NOT_ACTIVE;
            }
        } else {
            pthread_mutex_unlock(&buffer->cond_mutex);
            return NO_NEW_FRAME;
        }
    }
    // try to acquire read lock, if that fails, then we are busy
    while (pthread_rwlock_tryrdlock(&metadata->rwlock) != 0) {
        pthread_cond_wait(&buffer->cond, &buffer->cond_mutex);
        if (!buffer->is_alive) {
            pthread_mutex_unlock(&buffer->cond_mutex);
            return BLOCK_NOT_ACTIVE;
        }
    }  // we have acquired permissions at this point.
    // if the framework is dead, then cleanly exit

    pthread_mutex_unlock(&buffer->cond_mutex);

    // read from the frame
    size_t image_size = frame_image_size(frame);
    frame->frame_uid = metadata->frame_uid;
    frame->acquisition_time = metadata->acquisition_time;
    memcpy(frame->data, &buffer->images[image_size * target_buffer], image_size * sizeof(unsigned char));

    pthread_rwlock_unlock(&metadata->rwlock);
    return SUCCESS;
}

frame_t* create_frame() {
    frame_t* frame = (frame_t*)malloc(sizeof(frame_t));
    frame->data = (unsigned char*)malloc(sizeof(unsigned char) * 8);
    frame->frame_uid = 0ull;
    return frame;
}

void delete_frame(frame_t* ptr) {
    free(ptr->data);
    free(ptr);
}

// takes in [direction] and returns [BLOCK_DIR]-[direction]
char* file_address_from_direction(const char* direction) {
    // Check if "/"  exists in direction. Note that "/" is the ONLY forbidden 8 bit character
    // in Linux filenames
    if (strpbrk(direction, "/") != NULL) {
        fprintf(stderr, "Direction name \"%s\" contains a \"/\" which is forbidden!", direction);
        return NULL;
    }

    char* file_address = (char*)malloc(sizeof(char) * (strlen(direction) + strlen(BLOCK_DIR) + 1));
    strcpy(file_address, BLOCK_DIR);
    strcat(file_address, direction);
    return file_address;
}

// this is a helper
block_t* new_block(char* filename, buffer_t* buffer) {
    block_t* new_block = (block_t*)malloc(sizeof(block_t));
    new_block->buffer = buffer;
    new_block->filename = filename;
    return new_block;
}

block_t* create_block(const char* direction, size_t width, size_t height, size_t depth) {
    char* file_address = file_address_from_direction(direction);
    if (file_address == NULL) return NULL;

    // access evaluates to 0 if the file exists, else it evaluates to -1
    if (access(file_address, F_OK) == 0) {
        fprintf(stderr,
                "Buffer name \"%s\" already exists. You should destroy\
 the buffer before reusing it.",
                file_address);
        free(file_address);
        return NULL;
    }

    // file is open for read and write
    // file owner has read, write, and execute permissions.
    int buffer_file = open(file_address, O_RDWR | O_CREAT, S_IRWXU);
    size_t bytes_needed = buffer_size(width, height, depth);
    if (buffer_file == -1) {
        fprintf(stderr, "Failed to open file \"%s\" with error: %s.", file_address, strerror(errno));
        return NULL;
    }
    if (ftruncate(buffer_file, bytes_needed) == -1) {
        fprintf(stderr, "Failed to truncate the file to the desired length: %s.", strerror(errno));
        return NULL;
    }

    buffer_t* buffer = (buffer_t*)mmap(NULL, bytes_needed, PROT_READ | PROT_WRITE, MAP_SHARED,
                                       buffer_file, 0);

    buffer->frame_cnt = 0ull;
    close(buffer_file);
    buffer->width = width;
    buffer->height = height;
    buffer->depth = depth;
    buffer->owner = getpid();
    buffer->is_alive = true;

    pthread_condattr_t attrcond;
    pthread_condattr_init(&attrcond);
    pthread_condattr_setpshared(&attrcond, PTHREAD_PROCESS_SHARED);
    pthread_cond_init(&buffer->cond, &attrcond);

    pthread_mutexattr_t attrmutex;
    pthread_mutexattr_init(&attrmutex);
    pthread_mutexattr_setpshared(&attrmutex, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&buffer->cond_mutex, &attrmutex);

    pthread_rwlockattr_t attrrwlock;
    pthread_rwlockattr_init(&attrrwlock);
    pthread_rwlockattr_setpshared(&attrrwlock, PTHREAD_PROCESS_SHARED);
    for (int i = 0; i < BUFFER_COUNT; i++)
        pthread_rwlock_init(&buffer->metadata[i].rwlock, &attrrwlock);

    return new_block(file_address, buffer);
}

block_t* open_block(const char* direction) {
    char* file_address = file_address_from_direction(direction);

    // assert file_preconditions
    if (file_address == NULL) {
        return NULL;
    }

    // file is open for read and write
    // file owner has read, write, and execute permissions.
    int buffer_file = open(file_address, O_RDWR, S_IRWXU);

    if (buffer_file == -1) {
        fprintf(stderr, "Failed to create block access point because file \"%s\" dne.", file_address);
        return NULL;
    }

    size_t bytes_needed = lseek(buffer_file, 0, SEEK_END);
    buffer_t* buffer = (buffer_t*)mmap(NULL, bytes_needed, PROT_READ | PROT_WRITE, MAP_SHARED,
                                       buffer_file, 0);

    return new_block(file_address, buffer);
}

bool cstr_block_is_poisoned(const char* direction) {
    block_t* block = open_block(direction);
    bool result = block_is_poisoned(block);
    close_block(block);
    return result;
}

bool block_is_poisoned(const block_t* block) {
    buffer_t* buffer = block->buffer;
    // this line checks if the prev owner is still alive.
    // "On success (at least one signal was sent), zero is returned. On error, -1 is returned"
    // it will not actually kill the previous owner, since the signal is 0
    bool owner_is_alive = !kill(buffer->owner, 0);
    bool is_poisoned = !owner_is_alive && buffer->is_alive;
    if (is_poisoned) fprintf(stderr, "buffer at %s is poisoned.", block->filename);
    return is_poisoned;
}

bool cstr_block_is_alive(const char* direction) {
    block_t* block = open_block(direction);
    bool result = block_is_alive(block);
    close_block(block);
    return result;
}

bool block_is_alive(const block_t* block) {
    return block->buffer->is_alive;
}

void close_block(block_t* block) {
    buffer_t* buffer = block->buffer;
    if (buffer->owner == getpid()) {
        fprintf(stderr,
                "The current process with PID: %d owns the unlying buffer at %s.\
please call \"destroy_block\" instead.",
                getpid(), block->filename);
        return;
    }
    free(block->filename);
    free(block);
}

void destroy_block(block_t* block) {
    // check status
    bool is_owner = getpid() == block->buffer->owner;
    bool is_poisoned = block_is_poisoned(block);

    if (!is_owner && !is_poisoned) {
        fprintf(stderr,
                "Current process PID: %d cannot free unpoisoned block at %s owned by %d.",
                getpid(),
                block->filename,
                block->buffer->owner);
    }

    // kill!
    // there are two cases (owner, _) and (not ownwer, poisoned)..in both cases
    // we do not need to worry about owner-related race conditions because 1)
    // we are the owner, and 2) the owner is dead.

    buffer_t* buffer = block->buffer;
    buffer->is_alive = false;

    // start destruction process
    pthread_mutex_lock(&buffer->cond_mutex);

    // rename the filename so processes cannot access it during this vulnerable state
    char* archived = "-archived-random-name-so-no-direction-can-ever-be-like-this";
    char* new_filename = (char*)
        malloc(sizeof(char) * (strlen(block->filename) + strlen(archived) + 1));
    strcpy(new_filename, block->filename);
    strcat(new_filename, archived);
    if (rename(block->filename, new_filename) != 0) {
        fprintf(stderr,
                "file \"%s\" could not be archived during destruction process.\
 Segfaults may occur!",
                block->filename);
    }

    pthread_cond_broadcast(&buffer->cond);
    pthread_mutex_unlock(&buffer->cond_mutex);

    // sleep for 1 second to allow all watcher threads to clean up
    munmap(buffer, buffer_size(buffer->width, buffer->height,
                               buffer->depth));
    remove(new_filename);  // buffer does not exist after this
    free(new_filename);
    free(block->filename);
    free(block);
}