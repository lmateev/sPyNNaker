#ifndef _POST_EVENTS_H_
#define _POST_EVENTS_H_

// Standard includes
#include <stdbool.h>
#include <stdint.h>

// Include debug header for log_info etc
#include <debug.h>

//---------------------------------------
// Macros
//---------------------------------------
#define MAX_POST_SYNAPTIC_EVENTS 4
// This macro defines how much extra history trace space we allocate. However,
// extra space is not tied to particular neurons and therefore any neuron can steal
// space from other neurons that do not use their extra space.
#define EXTRA_HISTORY_TRACE_SPACE_FACTOR 3

//---------------------------------------
// Structures
//---------------------------------------
typedef struct {
    uint32_t count_minus_one;

    uint32_t* times;
    post_trace_t* traces;
    uint16_t size;                  // Total size of both times+traces arrays.
} post_event_history_t;

typedef struct {
    post_trace_t prev_trace;
    uint32_t prev_time;
    const post_trace_t *next_trace;
    const uint32_t *next_time;
    uint32_t num_events;
} post_event_window_t;

uint16_t TRACE_SIZE = sizeof(post_trace_t) + sizeof(uint32_t);

// Garbage collection include (Note: Leave it below preceding structs as they are used
// inside sppinn_gc.h).
#include "spinn_gc.h"

//---------------------------------------
// Inline functions
//---------------------------------------
static inline post_event_history_t *post_events_init_buffers(uint32_t n_neurons) {

    // Make trace size word aligned.
    while (TRACE_SIZE % 4 != 0)
      TRACE_SIZE++;

    post_event_history_t *post_event_history =
        (post_event_history_t*) spin1_malloc(
            n_neurons * sizeof(post_event_history_t));

    void* post_event_data =
            spin1_malloc(n_neurons * MAX_POST_SYNAPTIC_EVENTS * TRACE_SIZE);

    post_event_buffers.start_address = post_event_data;
    post_event_buffers.n_neurons = n_neurons;
    post_event_buffers.buffers = post_event_history;
    post_event_buffers.end_of_last_buffer = post_event_data + n_neurons * MAX_POST_SYNAPTIC_EVENTS * TRACE_SIZE;

    // Set internal pointers of each buffer
    for (int i = 0; i < n_neurons; i++) {
      post_event_history[i].times = post_event_data;
      post_event_history[i].traces = (void*)post_event_history[i].times + MAX_POST_SYNAPTIC_EVENTS*sizeof(uint32_t);
      post_event_data += MAX_POST_SYNAPTIC_EVENTS * TRACE_SIZE;
    }

    // Allocate extra space below post event data block for extensions.
    // **NOTE: For now giving 2 extra traces for each neuron but this needs
    // to be calculated properly when the rate of compaction will be known.
    block_t* extra_space = spin1_malloc(n_neurons * EXTRA_HISTORY_TRACE_SPACE_FACTOR * TRACE_SIZE);

    post_event_buffers.size = n_neurons * (MAX_POST_SYNAPTIC_EVENTS + EXTRA_HISTORY_TRACE_SPACE_FACTOR) * TRACE_SIZE;

    // Allocate compactor working space in SDRAM
    address_in_sdram = (int*) sark_xalloc (sv->sdram_heap, post_event_buffers.size, 0, 1);

    // Check allocations succeeded
    if (post_event_history == NULL || post_event_data == NULL || extra_space == NULL) {
        log_error("Unable to allocate global STDP structures - Out of DTCM");
        return NULL;
    }

    // Loop through neurons
    for (uint32_t n = 0; n < n_neurons; n++) {

        // Add initial placeholder entry to buffer
        post_event_history[n].times[0] = 0;
        post_event_history[n].traces[0] = timing_get_initial_post_trace();
        post_event_history[n].count_minus_one = 0;
        // Add initial size of the history trace buffer of this neuron.
        post_event_history[n].size = MAX_POST_SYNAPTIC_EVENTS * TRACE_SIZE;
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
 
    bool shift_elements = false;
    if (events -> times + events -> count_minus_one + 1 == events -> traces)
       shift_elements = !extend_hist_trace_buffer(&post_event_buffers, events);

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

    // Add buffer to specific generation if this is the first trace added.
    if (events -> count_minus_one == 1)
      generations[(int)ceil(time/generation_step)].buffers_in_generation[generations[(int)ceil(time/generation_step)].buffers_added++] = events;
}

#endif  // _POST_EVENTS_H_
