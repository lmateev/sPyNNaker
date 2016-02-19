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

#include <stdint.h>

/*!

We store references to live objects as indices in relation to the
start of a some particular data structure address.

E.g. if object is stored at location 0x400008 and it is part of
array with a top address 0x400000 then the object entry in this
vector will be 0x8, i.e. a size of all objects before it, in bytes.

*/
typedef struct {
  uint16_t *object_indices;          // An offset to an object
  uint16_t *object_sizes;            // Size of objects, in bytes.
  uint32_t start_address;            // The start address of the structure
  uint16_t size;                     // Overall buffer size.
  uint32_t n_neurons;                // Number of neurons in simulation
} vector_t;


//------------------------------------------------------------------------------
// Useful debug functions


/*

Print out the memory contents of the given addresses in hex and ASCII.
Each line contains 16 bytes of memory.

*/

void spinn_print_mem (char *start, char *end);

/*

Print free block regions that can be found on DTCM heap.

*/
void print_all_free_DTCM_heap_blocks ();

/*

Return the size of a block on DTCM heap in bytes.

NOTE: Size includes additional size of a block_t structure that is attached
to each object on the heap.

*/
uint sizeof_dtcm_block (block_t * pointer);


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
void sark_block_copy (int dest, const int src, uint n);

/*

Initialize vector of live objects and shadow vector.

*/
void init_gc_vectors (vector_t **vec1, vector_t **vec2, int n_neurons,
                      void* buff_addr, void* buff_top);

/*

Given a structure of history traces and a vector of live objects of each neuron
copy all objects to sdram into  a single continuous block. Store new indices to
a shadow_vec and interchange it with live_objects_vec at the end.

*/
void compact_post_traces (vector_t **live_objects, vector_t **shadow_vec);


/*

Move a buffer of history traces to the end of the strucutre
of history trace buffers. Update the index and size of the
relocated buffer.

Returns address of the buffer.

*/
void *extend_hist_trace_buffer (vector_t *live_objects,
                                int move_neuron_index,
                                int extend_by);

//------------------------------------------------------------------------------

#endif
