//------------------------------------------------------------------------------
//
// spinn_gc.h       Header file for SpiNNaker garbage collector structures
//
// Copyright (C)    The University of Manchester - 2015-2016
//
// Author           Mantas Mikaitis
//
//------------------------------------------------------------------------------

#ifndef SPINN_GC
#define SPINN_GC

#include "post_events.h"
#include "../../profiler.h"


//------------------------------------------------------------------------------
// Various debugging routines mainly to print out memory contents or
// certain DTCM heap structure pointer values.
//------------------------------------------------------------------------------

/*

Print out the memory contents of the given addresses in hex and ASCII.
Each line contains 16 bytes of memory.

*/
static inline void spinn_print_mem (char *start, char *end) {

  io_printf(IO_BUF, "\nPrinting memory. Start: %x End %x \n", start, ++end);

  while (start <= end) {
    io_printf (IO_BUF, "%x: ", start);

    int n = 0;

    // Loop through each block of next 16 bytes and print
    // them out in HEX.
    while (n < 16) {
      io_printf (IO_BUF, "%02x ", *(start+n));
      n++;
    }

    // Loop through each block of next 16 bytes and print
    // them out in ASCII.
    n = 0;
    io_printf (IO_BUF, "  ");
    while (n < 16) {
      if (*(start+n) < 0x80 && *(start+n) > 0x1F)
        io_printf (IO_BUF, "%2c", *(start+n));
      else
        io_printf (IO_BUF, ".");
      n++;
    }

    // Point to next block
    start += 16;
    io_printf (IO_BUF, "\n");
  }

}


/*

Print free block regions that can be found on DTCM heap.

*/
static inline void print_all_free_DTCM_heap_blocks () {

    block_t *next_free = sark.heap -> free;

    io_printf (IO_BUF, "\nThese are the free blocks on DTCM heap: \n");

    while (next_free != NULL) {
        io_printf (IO_BUF, "%x -- %x \n", next_free, next_free -> next);
        next_free = next_free -> free;
    }

}


/*

Return the size of a block on DTCM heap in bytes.

NOTE: Size includes additional size of a block_t structure that is attached
to each object on the heap.

*/
static inline uint sizeof_dtcm_block (block_t * pointer) {

  // Read block_t structure of the given block
  pointer -= 1;

  // Calculate size in bytes
  return ((pointer -> next) - pointer) * 8;

}



//------------------------------------------------------------------------------
// Garbage collection routines

/*

Using ARM Block Copy instructions, LDM/STM, copy given memory block
16 bytes or 4 words per instruction.

If number of words is not multiple of 4, then copy remaining bytes
one word at a time.

Inputs: dest, src : Destination and source of the copying.
        n         : Number of bytes to copy.

*/
static inline void sark_block_copy (int dest, const int src, uint n) {

  // Convert size n to number of words.
  if (n % 4 != 0)
    n = n / 4 + (n % 4);
  else
    n = n / 4;

  asm (
    "      LDR    r0, %[from]    \n\t"
    "      LDR    r1, %[to]      \n\t"
    "      MOV    r2, %[size]    \n\t"

    "blockcopy%=:                \n\t"
    "      MOVS   r3, r2, LSR #2 \n\t"
    "      BEQ    copywords%=    \n\t"
    "      PUSH   {r4-r7}        \n\t"

    "quadcopy%=:                 \n\t"
    "      LDMIA  r0!, {r4-r7}   \n\t"
    "      STMIA  r1!, {r4-r7}   \n\t"
    "      SUBS   r3, #1         \n\t"
    "      BNE    quadcopy%=     \n\t"
    "      POP    {r4-r7}        \n\t"

    "copywords%=:                \n\t"
    "      ANDS   r2, r2, #3     \n\t"
    "      BEQ    stop%=         \n\t"
    "wordcopy%=:                 \n\t"
    "      LDR    r3, [r0], #4   \n\t"
    "      STR    r3, [r1], #4   \n\t"
    "      SUBS   r2, r2, #1     \n\t"
    "      BNE    wordcopy%=     \n\t"

    "stop%=:                     \n\t"
    :: [to] "m" (dest),
       [from] "m" (src),
       [size] "r" (n)
    : "cc", "r0", "r1", "r2", "r3"
  );

}

/*

Given a structure of history traces and a vector of live objects of each neuron
copy all objects to sdram into  a single continuous block. Store new indices to
a shadow_vec and interchange it with live_objects_vec at the end.

*/
static inline void compact_post_traces (vector_t *live_objects_vec) {

  log_debug ("Memory compaction starts");
  profiler_write_entry_disable_irq_fiq(PROFILER_ENTER | PROFILER_COMPACT_POST_TRACES);

  // Allocate 32KB in SDRAM heap for work space.
  // **NOTE: Can also individually allocate space for each buffer in the for loop
  // below where exact size is known.
  int *address_in_sdram = (int*) sark_xalloc (sv->sdram_heap, 1024 * 32, 0, 1);
  if (address_in_sdram == NULL) {
    log_info ("Not enough memory in SDRAM");
  }

  log_debug ("Address in sdram allocated: %x", address_in_sdram);

  int *init_address = address_in_sdram;
  int overall_size = 0;

  // Copy live objects to SDRAM in a consecutive block
  for (int i = 0; i < live_objects_vec -> n_neurons; i++) {
    sark_block_copy (address_in_sdram,
                     (uint32_t)(live_objects_vec -> buffers)[i].times,
                     (live_objects_vec -> buffers)[i].size);

    overall_size += (live_objects_vec -> buffers)[i].size;

    (live_objects_vec -> buffers)[i].times =
      live_objects_vec -> start_address + ((int)address_in_sdram - (int)init_address);

    address_in_sdram =
      (int*) ((int)address_in_sdram + (live_objects_vec -> buffers)[i].size);
  }

  // Mark block in DTCM to detect when DMA completes.
  int *addr = (int*)(live_objects_vec -> start_address + overall_size - 4);
  addr[0] = -1;

  spin1_dma_transfer (2,
                      init_address,
                      (uint*)(live_objects_vec -> start_address),
                      DMA_READ,
                      overall_size);

  // Wait for marked memory location to be changed by DMA read.
  asm (
    "loop%=:                    \n\t"
    "      LDR    r3, %[mark]   \n\t"
    "      CMN    r3, #1        \n\t"
    "      BEQ    loop%=        \n\t"

    :: [mark] "m" (addr)
    : "cc", "r3"
  );

  sark_xfree (sv -> sdram_heap, init_address, 0);

  profiler_write_entry_disable_irq_fiq(PROFILER_EXIT | PROFILER_COMPACT_POST_TRACES);
}

/*

Move a buffer of history traces to the end of the strucutre
of history trace buffers. Update the index and size of the
relocated buffer.

Returns false if there is not enough space.

*/
static inline bool extend_hist_trace_buffer (vector_t *live_objects_vec,
                                post_event_history_t* buffer_to_move,
                                uint32_t move_neuron_index) {

  log_debug ("Post trace buffer extension starts");

  int last_buffer = 0;
  for (int i = 1; i < live_objects_vec -> n_neurons; i++) {
    if ((uint32_t)(live_objects_vec -> buffers)[i].times
        > (uint32_t)(live_objects_vec -> buffers)[last_buffer].times)
      last_buffer = i;
  }

  uint32_t end_of_last_buffer = (uint32_t)(live_objects_vec -> buffers)[last_buffer].times
                                + (live_objects_vec -> buffers)[last_buffer].size;

  // Check if there is enough space at the end of the heap to extend the buffer.
  if (live_objects_vec -> start_address + live_objects_vec -> size
      <= end_of_last_buffer + buffer_to_move -> size
         + TRACE_SIZE) {
    return false;
  }

  // Do not move the buffer if it is already at the end.
  if (move_neuron_index == last_buffer) {
    buffer_to_move -> size += TRACE_SIZE;
    return true;
  }

  // Copy the specified buffer to the end of all buffers.
  sark_block_copy (end_of_last_buffer,
                   buffer_to_move -> times,
                   buffer_to_move -> size);

  int traces_offset = (void*)buffer_to_move -> traces
                      - (void*)buffer_to_move -> times;
 
  buffer_to_move -> times = end_of_last_buffer;
  buffer_to_move -> traces = end_of_last_buffer + traces_offset; 
  buffer_to_move -> size += TRACE_SIZE;

  // Move traces down to make space for new time entry.
  if (sizeof(post_trace_t) != 0)
    for (int i = buffer_to_move -> count_minus_one; i >= 0; i--)
      (buffer_to_move -> traces)[i+1] = (buffer_to_move -> traces)[i];

  buffer_to_move -> traces += sizeof(post_trace_t);

  return true;
}

/*

Scan history traces and remove the ones that are older than the specified
oldest_time.

*/
static inline scan_history_traces (vector_t *live_objects_vec, int oldest_time) {

  log_debug("Recycling dead traces");

  for (int i = 0; i < live_objects_vec -> n_neurons; i++) {
    post_event_history_t* buffer = &(live_objects_vec -> buffers)[i];
    uint32_t recycled_traces = 0;
    if (buffer -> count_minus_one == 0)
      continue;

    for (int j = 0; j < buffer -> count_minus_one; j++) {
      if ((buffer -> times)[j] < oldest_time)
        recycled_traces++;
      else 
        break;
    }

    // Increment *times* pointer to drop oldest traces
    buffer -> times = (void*)buffer -> times + recycled_traces * 4;

    // Shift traces down in order to drop oldest trace entries that were recycled
    for (int i = 0; i <= buffer -> count_minus_one - recycled_traces; i++)
      (buffer -> traces)[i] = (buffer -> traces)[i+recycled_traces];

    buffer -> size -= recycled_traces * TRACE_SIZE;
    buffer -> count_minus_one = buffer -> count_minus_one - recycled_traces;
  }

}

//------------------------------------------------------------------------------

#endif
