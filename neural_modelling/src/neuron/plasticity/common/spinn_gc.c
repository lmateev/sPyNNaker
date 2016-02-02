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

//------------------------------------------------------------------------------
// Various debugging routines mainly to print out memory contents or
// certain DTCM heap structure pointer values.
//------------------------------------------------------------------------------


/*

Print out the memory contents of the given addresses in hex and ASCII.
Each line contains 16 bytes of memory.

*/
void spinn_print_dtcm_heap (char *start, char *end) {

  io_printf(IO_BUF, "\nPrinting memory. Start: %x End %x \n", start, ++end);

  while (start <= end) {
    io_printf (IO_BUF, "%x: ", start);

    int n = 0;

    // Loop through each block of next 16 bytes and print
    // them out in HEX.
    while (n < 16) {
      io_printf(IO_BUF, "%02x ", *(start+n));
      n++;
    }

    // Loop through each block of next 16 bytes and print
    // them out in ASCII.
    n = 0;
    io_printf(IO_BUF, "  ");
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
void print_all_free_DTCM_heap_blocks() {

    block_t *next_free = sark.heap -> free;

    io_printf(IO_BUF, "\nThese are the free blocks on DTCM heap: \n");

    while (next_free != NULL) {
        io_printf(IO_BUF, "%x -- %x \n", next_free, next_free -> next);
        next_free = next_free -> free;
    }

}


/*

Return the size of a block on DTCM heap in bytes.

NOTE: Size includes additional size of a block_t structure that is attached
to each object on the heap.

*/
uint sizeof_dtcm_block(block_t * pointer) {

  // Read block_t structu of the given block
  pointer -= 1;

  // Calculate size in bytes
  return ((pointer -> next) - pointer) * 8;

}


//------------------------------------------------------------------------------


/*

Using ARM Block Copy instructions, LDM/STM, copy given memory block
16 bytes per instruction.

If specified block size is not a multiple of 16 the remaining
number of bytes at the end are copied single byte at once.

Inputs: dest, src : Destination and source of the copying.
        n         : Size of the objects in bytes.

*/
uint sark_block_copy(void *dest, const void *src, uint n) {

  log_info("ARM block copy commences, dest: %x, src: %x, size: %u",
                   dest, src, n);

  asm (
    "      PUSH   {r0-r3}        \n\t"
    "      LDR    r0, %[from]    \n\t"
    "      LDR    r1, %[to]      \n\t"
    "      MOV    r2, %[size]    \n\t"

    "blockcopy%=:                \n\t"
    "      MOVS   r3, r2, LSR #4 \n\t"
    "      BEQ    copybytes%=    \n\t"
    "      PUSH   {r4-r7}        \n\t"

    "quadcopy%=:                 \n\t"
    "      LDM    r0!, {r4-r7}   \n\t"
    "      STM    r1!, {r4-r7}   \n\t"
    "      SUBS   r3, #1         \n\t"
    "      BNE    quadcopy%=     \n\t"
    "      POP    {r4-r7}        \n\t"

    "copybytes%=:                \n\t"
    "      ANDS   r2, r2, #15    \n\t"
    "      BEQ    stop%=         \n\t"
    "bytecopy%=:                 \n\t"
    "      LDRB   r3, [r0], #1   \n\t"
    "      STRB   r3, [r1], #1   \n\t"
    "      SUBS   r2, r2, #1     \n\t"
    "      BNE    bytecopy%=     \n\t"

    "stop%=:                     \n\t"
    "      POP    {r0-r3}        \n\t"
    :: [to] "m" (dest),      /* Address to copy block to */
       [from] "m" (src),     /* Address where block resides */
       [size] "r" (n)        /* Size in words */
  );

}


/*

Given a structure of history traces and a vector of live objects of each neuron
copy all objects to sdram into  a single continuous block. Create a new vector
of live objects which will store the indices to each history trace block as they
appear in a compacted block.

Returns a pointer to a new vector_t.

*/
uint copy_live_objects_to_sdram(int *traces, vector_t *live_objects, int *dest,
                                int n_neurons) {

  int i;
  int address_in_sdram = *dest;

  // Create a working vector of live objects which eventually will be
  // interchanged with the given live_objects.
  vector_t *live_objects_changed =          spin1_malloc(sizeof(vector_t));
  live_objects_changed -> object_indices =  spin1_malloc(n_neurons * sizeof(uint32_t));
  live_objects_changed -> object_sizes =    spin1_malloc(n_neurons * sizeof(uint32_t));

  for (i = 0; i < sizeof(live_objects -> object_indices); i++) {
    // Copy only those neuron trace buffers that have an entry in the vector
    // of live objects.
    if ((live_objects -> object_indices)[i] >= 0) {
      sark_block_copy(address_in_sdram,
                      (int)traces + (live_objects -> object_indices)[i],
                      (live_objects -> object_sizes)[i]);

      (live_objects_changed -> object_indices)[i] =
        (int) address_in_sdram;
      address_in_sdram += (live_objects -> object_sizes)[i];

    }
  }

  return live_objects_changed;
}

/*

Move a buffer of history traces to the end of the strucutre
of history trace buffers. Update the index and size of the
relocated buffer.

Returns address of the buffer.

*/
int extend_hist_trace_buffer(vector_t *live_objects,
                              int move_neuron_index,
                              int extend_by) {

  log_info("Buffer extension commences");

  // Get address next to the end of history trace structure.
  int last_buffer = 0;
  for (int i = 1; i < live_objects -> n_neurons; i++) {
    if ((live_objects -> object_indices)[i]
        > (live_objects -> object_indices)[last_buffer])
      last_buffer = i;
  }

  if (move_neuron_index == last_buffer)
    return (live_objects -> object_indices)[last_buffer]
           +  live_objects -> start_address;

  int end_of_buffer_structure = (live_objects -> object_indices)[last_buffer]
                                 +  live_objects -> start_address
                                 + (live_objects -> object_sizes)[last_buffer];

  // Copy the specified buffer to the end
  sark_block_copy(end_of_buffer_structure,
                  (live_objects -> object_indices)[move_neuron_index]
                  + live_objects -> start_address,
                  (live_objects -> object_sizes)[move_neuron_index]);

  // Update entry in the vector of live objects and the buffer's size.
  (live_objects -> object_indices)[move_neuron_index] = end_of_buffer_structure
                                                        - live_objects -> start_address;
  (live_objects -> object_sizes)[move_neuron_index] += extend_by;

  return end_of_buffer_structure;
}


/*

Copy block of memory from SDRAM to a given data structure.

*/
void dma_compact_live_objects(int *src, int *dest, int size) {

    log_info("DMA fired off.");

    spin1_dma_transfer(0,
                       *src,
                       *dest,
                       DMA_READ,
                       size);
}

//------------------------------------------------------------------------------
