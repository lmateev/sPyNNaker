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
        uint32_t n_neurons, vector_t *post_event_indices) {

    post_event_history_t *post_event_history =
        (post_event_history_t*) spin1_malloc(
            n_neurons * sizeof(post_event_history_t));

    // Allocate extra space for history traces in case we will overflow
    // maximum number of traces.
    // **NOTE**: For now giving 2 extra traces for each neuron but this needs
    // to be calculated accurately later on.
    spin1_malloc(n_neurons * 2 * (sizeof(uint32_t) + sizeof(post_trace_t)));

    // Check allocations succeeded
    if (post_event_history == NULL) {
        log_error("Unable to allocate global STDP structures - Out of DTCM");
        return NULL;
    }

    // Allocate space for the vector of indices
    post_event_indices -> object_indices = spin1_malloc(n_neurons * sizeof(uint32_t));
    post_event_indices -> object_sizes = spin1_malloc(n_neurons * sizeof(uint32_t));

    post_event_indices -> start_address = (int) post_event_history;
    post_event_indices -> n_neurons = n_neurons;

    // Loop through neurons
    for (uint32_t n = 0; n < n_neurons; n++) {

        // Add initial placeholder entry to buffer
        post_event_history[n].times[0] = 0;
        post_event_history[n].traces[0] = timing_get_initial_post_trace();
        post_event_history[n].count_minus_one = 0;

        // Initialize vector of live objects.

        // Add initial index of a history trace buffer of this neuron.
        (post_event_indices -> object_indices)[n] = n * sizeof(post_event_history_t);

        // Add initial size of the history trace buffer of this neuron.
        (post_event_indices -> object_sizes)[n] = sizeof(post_event_history_t);
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
                                   post_trace_t trace) {

        // If there's still space, store time at current end
        // and increment count minus 1
        const uint32_t new_index = ++events->count_minus_one;
        events->times[new_index] = time;
        events->traces[new_index] = trace;

}

#endif  // _POST_EVENTS_H_
