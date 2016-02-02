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
  uint16_t *object_indices;          // Pointer - START_OF_DTCM_HEAP
  uint16_t *object_sizes;            // Size of objects, in bytes.
  uint32_t start_address;            // The start address of the array
  uint32_t n_neurons;                // Number of neurons in simulation
} vector_t;


//------------------------------------------------------------------------------
// Useful debug functions


/*

Print out the memory contents of the given addresses in hex and ASCII.
Each line contains 16 bytes of memory.

*/

void spinn_print_dtcm_heap (char *start, char *end);

/*

Print free block regions that can be found on DTCM heap.

*/
void print_all_free_DTCM_heap_blocks();

/*

Return the size of a block on DTCM heap in bytes.

NOTE: Size includes additional size of a block_t structure that is attached
to each object on the heap.

*/
uint sizeof_dtcm_block(block_t * pointer);


//------------------------------------------------------------------------------
// Garbage collection routines

/*

Using ARM Block Copy instructions, LDM/STM, copy given memory block
16 bytes at a time.

If specified block size is not a multiple of 16 the remaining
number of bytes at the end are copied single byte at once.

Inputs: dest, src : Destination and source of the copying.
        n         : Size of the objects in bytes.

*/
uint sark_block_copy(void *dest, const void *src, uint n);

/*

Given a structure of history traces and a vector of live objects of each neuron
copy all objects to sdram into  a single continuous block. Create a new vector
of live objects which will store the indices to each history trace block as they
appear in a compacted block.

Returns a pointer to a new vector_t.

*/
uint copy_live_objects_to_sdram(int *traces, vector_t *live_objects, int *dest,
                                int n_neurons);


/*

Move a buffer of history traces to the end of the strucutre
of history trace buffers. Update the index and size of the
relocated buffer.

*/
int extend_hist_trace_buffer(vector_t *live_objects,
                              int move_neuron_index,
                              int extend_by);

/*

Copy block of memory from SDRAM to a given data structure.

*/
void dma_compact_live_objects(int *src, int *dest, int size);

//------------------------------------------------------------------------------

#endif
