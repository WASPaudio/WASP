/*
* ring_buffer.h — Single-producer single-consumer lock-free ring buffer
 * for passing MIDI events from input threads to the audio thread.
 */

#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#define RING_BUFFER_SIZE 256  /* must be power of 2 */
#define RING_BUFFER_MASK (RING_BUFFER_SIZE - 1)

typedef struct {
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
} MidiMessage;

typedef struct {
    MidiMessage  buffer[RING_BUFFER_SIZE];
    std::atomic_uint  write_pos;
    std::atomic_uint  read_pos;
} RingBuffer;

static inline void ring_buffer_init(RingBuffer* rb) {
    memset(rb, 0, sizeof(*rb));
    atomic_store(&rb->write_pos, 0);
    atomic_store(&rb->read_pos,  0);
}

/* Called from input thread. Returns false if full. */
static inline bool ring_buffer_push(RingBuffer* rb, MidiMessage msg) {
    unsigned write = atomic_load_explicit(&rb->write_pos, std::memory_order_relaxed);
    unsigned next  = (write + 1) & RING_BUFFER_MASK;
    if (next == atomic_load_explicit(&rb->read_pos, std::memory_order_acquire))
        return false; /* full */
    rb->buffer[write] = msg;
    atomic_store_explicit(&rb->write_pos, next, std::memory_order_release);
    return true;
}

/* Called from audio thread. Returns false if empty. */
static inline bool ring_buffer_pop(RingBuffer* rb, MidiMessage* msg) {
    unsigned read = atomic_load_explicit(&rb->read_pos, std::memory_order_relaxed);
    if (read == atomic_load_explicit(&rb->write_pos, std::memory_order_acquire))
        return false; /* empty */
    *msg = rb->buffer[read];
    atomic_store_explicit(&rb->read_pos,
                          (read + 1) & RING_BUFFER_MASK,
                          std::memory_order_release);
    return true;
}

#endif /* RING_BUFFER_H */