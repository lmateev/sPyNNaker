#ifndef _POST_EVENTS_H_
#define _POST_EVENTS_H_

// Standard includes
#include <stdbool.h>
#include <stdint.h>

// Include debug header for log_info etc
#include <debug.h>

// Garbage collection include
#include "spinn_gc.h"

//---------------------------------------
// Macros
//---------------------------------------
#define MAX_POST_SYNAPTIC_EVENTS 4

//---------------------------------------
// Structures
//---------------------------------------
typedef struct {
    uint32_t count_minus_one;

    uint32_t times[MAX_POST_SYNAPTIC_EVENTS];
    post_trace_t traces[MAX_POST_SYNAPTIC_EVENTS];
} post_event_history_t;

typedef struct {
    post_trace_t prev_trace;
    uint32_t prev_time;
    const post_trace_t *next_trace;
    const uint32_t *next_time;
    uint32_t num_events;
} post_event_window_t;

//---------------------------------------
// Inline functions
//---------------------------------------
static inline post_event_history_t *post_events_init_buffers(
        uint32_t n_neurons, vector_t **post_event_vec,
        vector_t **post_event_shadow_vec) {

    post_event_history_t *post_event_history =
        (post_event_history_t*) spin1_malloc(
            n_neurons * sizeof(post_event_history_t));

    // Allocate extra space for buffer extender.
    // **NOTE: For now giving 2 extra traces for each neuron but this needs
    // to be calculated properly when the rate of compaction will be known.
    block_t* extra_space = sark_alloc (
        n_neurons, 2 * (sizeof(uint32_t) + sizeof(post_trace_t)));

    // Then the last address of hist trace structure can be simply extracted
    // from block_t of extra space.
    uint32_t buffer_top_addr = (extra_space-1) -> next;

    // Check allocations succeeded
    if (post_event_history == NULL || extra_space == NULL) {
        log_error("Unable to allocate global STDP structures - Out of DTCM");
        return NULL;
    }

    init_gc_vectors (post_event_vec, post_event_shadow_vec,
                     n_neurons, post_event_history, buffer_top_addr);

    // Loop through neurons
    for (uint32_t n = 0; n < n_neurons; n++) {

        // Add initial placeholder entry to buffer
        post_event_history[n].times[0] = 0;
        post_event_history[n].traces[0] = timing_get_initial_post_trace();
        post_event_history[n].count_minus_one = 0;

        // Add initial index of a history trace buffer of this neuron.
        ((*post_event_vec) -> object_indices)[n] = n * sizeof(post_event_history_t);
        // Add initial size of the history trace buffer of this neuron.
        ((*post_event_vec) -> object_sizes)[n] = sizeof(post_event_history_t);
    }

    return post_event_history;
}

//---------------------------------------
static inline post_event_window_t post_events_get_window(
        const post_event_history_t *events, uint32_t begin_time) {

    // Start at end event - beyond end of post-event history
    const uint32_t count = events->count_minus_one + 1;
    const uint32_t *end_event_time = events->times + count;
    const post_trace_t *end_event_trace = events->traces + count;
    const uint32_t *event_time = end_event_time;
    post_event_window_t window;
    do {

        // Cache pointer to this event as potential
        // Next event and go back one event
        // **NOTE** next_time can be invalid
        window.next_time = event_time--;
    }

    // Keep looping while event occured after start
    // Of window and we haven't hit beginning of array
    while (*event_time > begin_time && event_time != events->times);

    // Deference event to use as previous
    window.prev_time = *event_time;

    // Calculate number of events
    window.num_events = (end_event_time - window.next_time);

    // Using num_events, find next and previous traces
    window.next_trace = (end_event_trace - window.num_events);
    window.prev_trace = *(window.next_trace - 1);

    // Return window
    return window;
}

//---------------------------------------
static inline post_event_window_t post_events_get_window_delayed(
        const post_event_history_t *events, uint32_t begin_time,
        uint32_t end_time) {

    // Start at end event - beyond end of post-event history
    const uint32_t count = events->count_minus_one + 1;
    const uint32_t *end_event_time = events->times + count;
    const uint32_t *event_time = end_event_time;

    post_event_window_t window;
    do {
        // Cache pointer to this event as potential
        // Next event and go back one event
        // **NOTE** next_time can be invalid
        window.next_time = event_time--;

        // If this event is still in the future, move the end time back
        if (*event_time > end_time) {
            end_event_time = window.next_time;
        }
    }

    // Keep looping while event occured after start
    // Of window and we haven't hit beginning of array
    while (*event_time > begin_time && event_time != events->times);

    // Deference event to use as previous
    window.prev_time = *event_time;

    // Calculate number of events
    window.num_events = (end_event_time - window.next_time);

    // Using num_events, find next and previous traces
    const post_trace_t *end_event_trace = events->traces + count;
    window.next_trace = (end_event_trace - window.num_events);
    window.prev_trace = *(window.next_trace - 1);

    // Return window
    return window;
}

//---------------------------------------
static inline post_event_window_t post_events_next(post_event_window_t window) {

    // Update previous time and increment next time
    window.prev_time = *window.next_time++;
    window.prev_trace = *window.next_trace++;

    // Decrement remining events
    window.num_events--;
    return window;
}

//---------------------------------------
static inline post_event_window_t post_events_next_delayed(
        post_event_window_t window, uint32_t delayed_time) {

    // Update previous time and increment next time
    window.prev_time = delayed_time;
    window.prev_trace = *window.next_trace++;

    // Go onto next event
    window.next_time++;

    // Decrement remining events
    window.num_events--;
    return window;
}

//---------------------------------------
static inline void post_events_add(uint32_t time, post_event_history_t *events,
                                   post_trace_t trace, bool shift_elements) {

    if (!shift_elements) {

        // If there's still space, store time at current end
        // and increment count minus 1
        const uint32_t new_index = ++events->count_minus_one;
        events->times[new_index] = time;
        events->traces[new_index] = trace;
    } else {

        // Otherwise Shuffle down elements
        // **NOTE** 1st element is always an entry at time 0
        for (uint32_t e = 2; e <= events -> count_minus_one; e++) {
            events->times[e - 1] = events->times[e];
            events->traces[e - 1] = events->traces[e];
        }

        // Stick new time at end
        events->times[events->count_minus_one] = time;
        events->traces[events->count_minus_one] = trace;
    }

}

#endif  // _POST_EVENTS_H_
