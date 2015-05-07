/*! \file
 *
 * \brief The implementation of a buffer for incoming spikes (in_spikes.c)
 *
 *
 *  SUMMARY
 *    Incoming spike handling for SpiNNaker neural modelling
 *
 *    The essential feature of the buffer used in this impementation is that it
 *    requires no critical-section interlocking --- PROVIDED THERE ARE ONLY TWO
 *    PROCESSES: a producer/consumer pair. If this is changed, then a more
 *    intricate implementation will probably be required, involving the use
 *    of enable/disable interrupts.
 *
 *  \author
 *    Dave Lester (david.r.lester@manchester.ac.uk)
 *
 *  \copyright
 *    Copyright (c) Dave Lester and The University of Manchester, 2013.
 *    All rights reserved.
 *    SpiNNaker Project
 *    Advanced Processor Technologies Group
 *    School of Computer Science
 *    The University of Manchester
 *    Manchester M13 9PL, UK
 *
 *  DETAILS
 *
 *    The producer uses the variable input to add items to the buffer; the
 *    consumer uses the variable output to remove items from the buffer.
 *
 *    In an event-based, or interrupt-driven system this allows us to perform
 *    both adding and removing items without the need for interrupts to be disabled.
 *
 *    The price to be paid is that we might only be able to buffer 255 items in a 256
 *    entry buffer.
 *
 *  \date
 *    10 December, 2013
 *
 *  HISTORY
 * *  DETAILS
 *    Created on       : 10 December 2013
 *    Version          : $Revision$
 *    Last modified on : $Date$
 *    Last modified by : $Author$
 *    $Id$
 *
 *    $Log$
 *
 */

#include "in_spikes.h"
#include <debug.h>

static spike_t* buffer;
static uint32_t buffer_size;

//! The consumer manipulates ouput, the producer manipulates "input".

static index_t   output;
static index_t   input;
static counter_t overflows;

//! \brief Increments an index

#define next(a) do {(a) = (((a)+1) == buffer_size)? 0: ((a)+1); } while (false)

//! \brief Looks at the next item in the buffer.
//! \return The index of the next item in the buffer to be output.

static inline index_t peek_next (void)
{   return ((output == 0)? buffer_size - 1: output - 1); }

//! \brief Calculates the difference between input and output, returning a
//! non-negative answer, which is less than the size of the buffer.
//! \return The difference between input and output.

static inline counter_t buffer_diff (void)
{
    register counter_t r = ((input > output)? 0: buffer_size) + input - output;

    assert (r < buffer_size);

    return (r);
}

//! \brief Returns a number of unallocated slots in the buffer.
//! There might actually be one more, if the consumer has not updated it's output pointer.
//!
//! \return A number of buffer slots currently unallocated.

static inline counter_t unallocated (void)
{   return (buffer_diff ()); }

//! \brief Returns a number of allocated slots in the buffer.
//! There might actually be one fewer, if the producer has not updated it's input pointer.
//!
//! \return A number of buffer slots currently allocated.

static inline counter_t allocated (void)
{   return (buffer_size - buffer_diff () - 1); }

//! \brief A non_empty buffer can have an item extracted by the consumer.
//! \return A bool indicating that an extraction operation may be performed
//! by the consumer.

static inline bool non_empty (void)
{   return (allocated () > 0); }

//! \brief A non_full buffer can have an item entered by the producer.
//! \return A bool indicating that an item may be entered into the buffer
//! by the producer.

static inline bool non_full (void)
{   return (unallocated () > 0); }

//! \brief Initialize the incoming spike buffer.
//! \param[in] size The maximum number of items in the spike buffer.
//! \return Whether the allocation of the buffer took place successfully.

bool in_spikes_initialize_spike_buffer (uint32_t size)
{
    assert (size > 0);

    buffer = (spike_t*) sark_alloc(1, size * sizeof (spike_t));

    check_dtcm (buffer);
    if (buffer == NULL) {
        log_error ("Cannot allocate in spikes buffer");
        return (false);
    }

    buffer_size = size;
    input       = size - 1;
    output      = 0;
    overflows   = 0;
    underflows  = 0;

    return (true);
}

//! \brief A synonym for "allocated"
//! \return A number of used items.

uint32_t in_spikes_n_spikes_in_buffer (void)
{   return (allocated ()); }

//! \brief Adds a spike to the buffer if this is possible.
//! \param[in] spike The incoming spike to be placed in the buffer.
//! \return Returns true if the spike is successfully placed into the buffer,
//! otherwise false.

bool in_spikes_add_spike (spike_t spike)
{
    bool success = non_full ();

    if (success) {
        buffer [input] = spike;
        next (input);
    } else
        overflows++;

    return (success);
}

//! \brief Gets a spike from the buffer; this is always possible.
//! \param[out] spike The address into which the spike is be written.
//! \return Returns true if the spike is extracted from the buffer,
//! otherwise false.

bool in_spikes_get_next_spike (spike_t* spike)
{
    bool success = non_empty ();

    assert (success); // A failure here indicates that we're extracting from an empty buffer.

    next (output);
    *spike = buffer [output];

    return (success);
}

//! \brief Gets a spike from the buffer, checking whether it matches the current one.
//! If it does, then the buffer is advanced without a DMA occuring.
//! \param[in] spike The spike address to be matched.
//! \return Returns true if the spike in the buffer matches the previous one,
//! otherwise false.

bool in_spikes_is_next_spike_equal (spike_t spike)
{
    bool success = non_empty ();

    assert (success); // A failure here indicates that we're extracting from an empty buffer.

    index_t peek_output = peek_next ();

    success = buffer [peek_output] == spike;

    if (success)
        output = peek_output;

    return (success);
}

//! \brief Provides access to the overflow counter.
//! \return The number of overflows that have occurred.

counter_t in_spikes_get_n_buffer_overflows (void)
{   return (overflows); }


#if LOG_LEVEL >= LOG_DEBUG

//! \brief A printer for the incoming spike buffer.

void in_spikes_print_buffer (void)
{
    counter_t n = allocated ();
    index_t a;

    log_debug("buffer: input = %3u, output = %3u elements = %3u\n", input,
              output, n);
    printf("------------------------------------------------\n");

    for (; n > 0; n--) {
        a = (input + n) % buffer_size;
        log_debug("  %3u: %08x\n", a, buffer[a]);
    }

    log_debug("------------------------------------------------\n");
}
#else // DEBUG

//! \brief A printer for the incoming spike buffer.

void in_spikes_print_buffer (void)
{   skip (); }
#endif // DEBUG
