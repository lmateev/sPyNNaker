//------------------------------------------------------------------------------
//
// spinn_gc.c       Main SpiNNaker garbage collection implementation
//
// Copyright (C)    The University of Manchester - 2015-2016
//
// Author           Mantas Mikaitis
//
//------------------------------------------------------------------------------

#include <sark.h>
#include <stdio.h>

#include "spin1_api.h"
#include <debug.h>

#include "./spinn_gc.h"
#include "../../profiler.h"

//------------------------------------------------------------------------------
// Various debugging routines mainly to print out memory contents or
// certain DTCM heap structure pointer values.
//------------------------------------------------------------------------------


/*

Print out the memory contents of the given addresses in hex and ASCII.
Each line contains 16 bytes of memory.

*/
void spinn_print_mem (char *start, char *end) {

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
void print_all_free_DTCM_heap_blocks () {

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
uint sizeof_dtcm_block (block_t * pointer) {

  // Read block_t structure of the given block
  pointer -= 1;

  // Calculate size in bytes
  return ((pointer -> next) - pointer) * 8;

}


//------------------------------------------------------------------------------


/*

Using ARM Block Copy instructions, LDM/STM, copy given memory block
16 bytes or 4 words per instruction.

If number of words is not multiple of 4, then copy remaining bytes
one word at a time.

Inputs: dest, src : Destination and source addresses.
        n         : Number of bytes to copy.

*/
void sark_block_copy (int dest, const int src, uint n) {

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

Initialize vectors of live objects.

*/
void init_gc_vectors (vector_t **vec1, vector_t **vec2, int n_neurons, void* buff_addr) {

  *vec1 = spin1_malloc (sizeof(vector_t));
  (*vec1) -> object_indices =  spin1_malloc (n_neurons * sizeof(uint32_t));
  (*vec1) -> object_sizes =    spin1_malloc (n_neurons * sizeof(uint32_t));
  (*vec1) -> n_neurons = n_neurons;
  (*vec1) -> start_address = (int) buff_addr;

  *vec2 = spin1_malloc (sizeof(vector_t));
  (*vec2) -> object_indices =  spin1_malloc (n_neurons * sizeof(uint32_t));
  (*vec2) -> object_sizes =    spin1_malloc (n_neurons * sizeof(uint32_t));
  (*vec2) -> n_neurons = n_neurons;
  (*vec2) -> start_address = (int) buff_addr;


}


/*

Given a structure of history traces and a vector of live objects of each neuron
copy all objects to sdram into  a single continuous block. Store new indices to
a shadow_vec and interchange it with live_objects_vec at the end.

*/
void compact_post_traces (vector_t **live_objects_vec, vector_t **shadow_vec) {

  log_info ("Memory compaction starts");
  profiler_write_entry_disable_irq_fiq(PROFILER_ENTER | PROFILER_COMPACT_POST_TRACES);

  // Allocate 32KB in SDRAM heap for work space.
  // **NOTE: Can also individually allocate space for each buffer in the for loop
  // below where exact size is known.
  int *address_in_sdram = (int*) sark_xalloc (sv->sdram_heap, 1024 * 32, 0, 1);
  log_debug ("Address in sdram allocated: %x", address_in_sdram);

  int *init_address = address_in_sdram;
  int overall_size = 0;

  // Copy live objects to SDRAM in a consecutive block
  for (int i = 0; i < (*live_objects_vec) -> n_neurons; i++) {
    sark_block_copy ((int)address_in_sdram,
                     (int)(*live_objects_vec) -> start_address + ((*live_objects_vec) -> object_indices)[i],
                     ((*live_objects_vec) -> object_sizes)[i]);
    overall_size += ((*live_objects_vec) -> object_sizes)[i];

    ((*shadow_vec) -> object_indices)[i] = (int)address_in_sdram - (int)init_address;
    ((*shadow_vec) -> object_sizes)[i] = ((*live_objects_vec) -> object_sizes)[i];

    address_in_sdram = (int*) ((int)address_in_sdram + ((*live_objects_vec) -> object_sizes)[i]);
  }

  // Mark block in DTCM to detect when DMA completes.
  int *addr = (int*)((*live_objects_vec) -> start_address + overall_size - 4);
  addr[0] = -1;

  spin1_dma_transfer (0,
                      init_address,
                      (uint*)(*live_objects_vec) -> start_address,
                      DMA_READ,
                      overall_size);

  int count = 0;

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

  vector_t *tmp = *live_objects_vec;
  *live_objects_vec = *shadow_vec;
  *shadow_vec = tmp;

  profiler_write_entry_disable_irq_fiq(PROFILER_EXIT | PROFILER_COMPACT_POST_TRACES);
}

/*

Move a buffer of history traces to the end of the strucutre
of history trace buffers. Update the index and size of the
relocated buffer.

Returns address of the buffer.

*/
void *extend_hist_trace_buffer (vector_t *live_objects_vec,
                                int move_neuron_index,
                                int extend_by) {

  log_info ("Post trace buffer extension starts");

  int last_buffer = 0;
  for (int i = 1; i < live_objects_vec -> n_neurons; i++) {
    if ((live_objects_vec -> object_indices)[i]
        > (live_objects_vec -> object_indices)[last_buffer])
      last_buffer = i;
  }

  // Do not extend if move_neuron_index is at the end of buffer structure.
  if (move_neuron_index == last_buffer) {
    (live_objects_vec -> object_sizes)[move_neuron_index] += extend_by;
    return (void*)((live_objects_vec -> object_indices)[last_buffer]
                   + live_objects_vec -> start_address);
  }

  uint32_t end_of_buffer_structure = (live_objects_vec -> object_indices)[last_buffer]
                                 +  live_objects_vec -> start_address
                                 + (live_objects_vec -> object_sizes)[last_buffer];

  // Copy the specified buffer to the end of all buffers.
  sark_block_copy (end_of_buffer_structure,
                   (live_objects_vec -> object_indices)[move_neuron_index]
                   + live_objects_vec -> start_address,
                   (live_objects_vec -> object_sizes)[move_neuron_index]);

  (live_objects_vec -> object_indices)[move_neuron_index] =
      end_of_buffer_structure - (live_objects_vec -> start_address);
  (live_objects_vec -> object_sizes)[move_neuron_index] += extend_by;

  return (void*)end_of_buffer_structure;
}

//------------------------------------------------------------------------------
