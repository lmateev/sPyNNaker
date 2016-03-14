//------------------------------------------------------------------------------
//
// spinn_gc.h       Header file for SpiNNaker garbage collector structures
//
// Copyright (C)    The University of Manchester - 2015-2016
//
// Author           Mantas Mikaitis
//
//------------------------------------------------------------------------------

#define COMPACTOR_FRAGMENTATION_FACTOR 4
#define GENERATIONS_TO_USE 8

#ifndef SPINN_GC
#define SPINN_GC

#include "post_events.h"
#include "../../profiler.h"

typedef struct {
  uint32_t start_address;            // Top of buffer structure
  uint16_t size;                     // Overall size of all buffers
  uint32_t n_neurons;
  uint32_t end_of_last_buffer;
  post_event_history_t* buffers;
} post_event_buffer_t;

post_event_buffer_t post_event_buffers;
void* address_in_sdram;              // Working space for compactor.

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
copy all objects to sdram into a single continuous block and copy back to DTCM
in order to compact. The compaction is each time working on some part of the
memory region and the region is moving after each compaction invocation. The region
eventually returns to the initial address and the cycle repeats.
The size of regions to compact is controlled by macro COMPACTOR_FRAGMENTATION_FACTOR.

*/
static inline void compact_post_traces (post_event_buffer_t *post_event_buffers) {

  static uint32_t start_addr = 0;
  static uint32_t end_addr = 0;
  static uint32_t end_of_last_compaction_address = 0;

  profiler_write_entry_disable_irq_fiq(PROFILER_ENTER | PROFILER_COMPACT_POST_TRACES);

  // Define region which compactor will work on in this invocation.
  if (start_addr == 0) {
    start_addr = post_event_buffers -> start_address;
    end_addr = post_event_buffers -> start_address
               + (post_event_buffers -> size / COMPACTOR_FRAGMENTATION_FACTOR);
    end_of_last_compaction_address = start_addr;
  }
  else {
    start_addr += post_event_buffers -> size / COMPACTOR_FRAGMENTATION_FACTOR;
    end_addr += post_event_buffers -> size / COMPACTOR_FRAGMENTATION_FACTOR;
  }

  int *init_address = address_in_sdram;
  int overall_size = 0;

  // Copy live objects to SDRAM in a consecutive block
  for (int i = 0; i < post_event_buffers -> n_neurons; i++) {
    if ((post_event_buffers -> buffers)[i].times >= start_addr
        & (post_event_buffers -> buffers)[i].times <= end_addr) {
	    sark_block_copy (address_in_sdram,
			     (post_event_buffers -> buffers)[i].times,
			     (post_event_buffers -> buffers)[i].size);

	    int traces_offset = (void*)(post_event_buffers -> buffers)[i].traces
				- (void*)(post_event_buffers -> buffers)[i].times;

	    // Update references.
	    (post_event_buffers -> buffers)[i].times =
	      end_of_last_compaction_address + overall_size;
	    (post_event_buffers -> buffers)[i].traces =
	      (void *)(post_event_buffers -> buffers)[i].times + traces_offset;
	    overall_size += (post_event_buffers -> buffers)[i].size;

	    address_in_sdram =
	      (int*) ((int)address_in_sdram + (post_event_buffers -> buffers)[i].size);
    }
  }

  address_in_sdram = init_address;

  // Mark block in DTCM to detect when DMA completes.
  int *addr = (int*)(end_of_last_compaction_address + overall_size - 4);
  addr[0] = -1;

  spin1_dma_transfer (2,
                      init_address,
                      end_of_last_compaction_address,
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

  end_of_last_compaction_address += overall_size;

  // Finish compaction if we have already moved last buffer.
  if (end_addr >= post_event_buffers -> end_of_last_buffer) {
    post_event_buffers -> end_of_last_buffer = end_of_last_compaction_address;
    start_addr = 0;
  }

  profiler_write_entry_disable_irq_fiq(PROFILER_EXIT | PROFILER_COMPACT_POST_TRACES);
}

/*

Move a buffer of history traces to the end of the strucutre
of history trace buffers. Update the index and size of the
relocated buffer.

Returns false if there is not enough space.

*/
static inline bool extend_hist_trace_buffer (post_event_buffer_t *post_event_buffers,
                                             post_event_history_t* buffer_to_move) {

  profiler_write_entry(PROFILER_ENTER | PROFILER_EXTEND_POST_BUFFER);

  // Check if there is enough space at the end of the heap to extend the buffer.
  if (post_event_buffers -> start_address + post_event_buffers -> size
      <= (post_event_buffers -> end_of_last_buffer + buffer_to_move -> size
          + TRACE_SIZE)) {
    profiler_write_entry_disable_irq_fiq(PROFILER_EXIT | PROFILER_EXTEND_POST_BUFFER);
    return false;
  }

  if ((void*)buffer_to_move -> times + buffer_to_move -> size !=
      post_event_buffers -> end_of_last_buffer) {
    // Copy the specified buffer to the end of all buffers.
    sark_block_copy (post_event_buffers -> end_of_last_buffer,
                     buffer_to_move -> times,
                     buffer_to_move -> size);

    int traces_offset = (void*)buffer_to_move -> traces
                        - (void*)buffer_to_move -> times;
    // Update pointers to the (now moved) data structure.
    buffer_to_move -> times = post_event_buffers -> end_of_last_buffer;
    buffer_to_move -> traces = post_event_buffers -> end_of_last_buffer + traces_offset;
  }

  if (sizeof(post_trace_t) != 0)
    // Move traces down to make space for new time entry.
    for (int i = buffer_to_move -> count_minus_one; i >= 0; i--)
      (buffer_to_move -> traces)[i+(sizeof(uint32_t)/sizeof(post_trace_t))] = (buffer_to_move -> traces)[i];

  // Update buffer pointers and size
  buffer_to_move -> traces = (void*)buffer_to_move -> traces + sizeof(uint32_t);
  buffer_to_move -> size += TRACE_SIZE;
  post_event_buffers -> end_of_last_buffer = (void*)buffer_to_move -> times + buffer_to_move -> size;

  profiler_write_entry(PROFILER_EXIT | PROFILER_EXTEND_POST_BUFFER);

  return true;
}

/*

Scan history traces and remove traces that are older than the oldest specified time.
The removal is done by moving the pointer to *times* structure down. This way, the
compactor will recycle the trace that we do not point to anymore.

*/
static inline scan_history_traces (post_event_buffer_t *post_event_buffers, int oldest_time) {

  profiler_write_entry(PROFILER_ENTER | PROFILER_SCAN_POST_BUFFER);

  for (int i = 0; i < post_event_buffers -> n_neurons; i++) {
    post_event_history_t* buffer = &(post_event_buffers -> buffers)[i];
    uint32_t recycled_traces = 0;

    for (int j = 0; j < buffer -> count_minus_one; j++) {
      if ((buffer -> times)[j] < oldest_time)
        recycled_traces++;
      else 
        break;
    }

    if (recycled_traces == 0)
      continue;

    // Increment *times* pointer to drop oldest traces
    buffer -> times = buffer -> times + recycled_traces;

    // Shift traces down in order to drop oldest trace entries that were recycled
    for (int i = 0; i <= buffer -> count_minus_one - recycled_traces; i++)
      (buffer -> traces)[i] =
        (buffer -> traces)[i+recycled_traces];

    buffer -> size -= recycled_traces * TRACE_SIZE;
    buffer -> count_minus_one -= recycled_traces;

  }

  profiler_write_entry(PROFILER_EXIT | PROFILER_SCAN_POST_BUFFER);

}

//------------------------------------------------------------------------------

#endif
