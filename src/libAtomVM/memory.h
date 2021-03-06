/***************************************************************************
 *   Copyright 2018 by Davide Bettio <davide@uninstall.it>                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Lesser General Public License as        *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA .        *
 ***************************************************************************/

#ifndef _MEMORY_H_
#define _MEMORY_H_

#include "utils.h"

#include <stdint.h>

#define HEAP_NEED_GC_SHRINK_THRESHOLD_COEFF 64

#ifndef TYPEDEF_TERM
#define TYPEDEF_TERM
typedef unsigned long term;
#endif

#ifndef TYPEDEF_CONTEXT
#define TYPEDEF_CONTEXT
typedef struct Context Context;
#endif

enum MemoryGCResult
{
    MEMORY_GC_OK = 0,
    MEMORY_GC_ERROR_FAILED_ALLOCATION = 1
};

enum MemoryEstimateResult
{
    MEMORY_ESTIMATE_OK = 0,
    MEMORY_ESTIMATE_FAILED_ALLOCATION = 1
};

/**
 * @brief allocates space for a certain ammount of terms on the heap
 *
 * @details allocates space for a certain ammount of terms on the heap, GC will be performed when needed, any existing term might be invalid after this call.
 * @param ctx the context that owns the heap.
 * @param size the ammount of terms that will be allocated.
 * @returns a pointer to the newly allocated memory block.
 */
MALLOC_LIKE term *memory_heap_alloc(Context *ctx, uint32_t size);

/**
 * @brief allocates a new memory block and executes garbage collection
 *
 * @details allocates a new memory block (that can have new size) and executes garbage collection, any existing term might be invalid after this call.
 * @param ctx the context that owns the memory block.
 * @param new_size the size of the new memory block in term units.
 * @returns MEMORY_GC_OK when successful.
 */
enum MemoryGCResult memory_gc(Context *ctx, int new_size);

/**
 * @brief copies a term to a destination heap
 *
 * @details deep copies a term to a destination heap, once finished old memory can be freed.
 * @param move if set to 1 a destructive copy will be executed, this operation tries to keep shared terms.
 * @param new_heap the destination heap where terms will be copied.
 * @param new_stack the stack that will be used during term copy.
 * @returns a new term that is stored on the new heap.
 */
term memory_copy_term_tree(term **new_heap, term **new_stack, term t, int move);

/**
 * @brief meakes sure that the given context has given free memory
 *
 * @details this function makes sure that at least size terms are available, when not available gc will be performed, any existing term might be invalid after this call.

 * @param ctx the target context.
 * @param size needed available memory.
 */
void memory_ensure_free(Context *ctx, uint32_t size);

/**
 * @brief runs a garbage collection and shrinks used memory
 *
 * @details runs a garbage collection and shrinks used memory, a new heap will be allocted, any existing term might be invalid after this call.
 * @param ctx the context on which the garbage collection will be performed.
 */
void memory_gc_and_shrink(Context *ctx);

/**
 * @brief calculates term memory usage
 *
 * @details perform an used memory calculation using given term as root, shared memory (that is not part of the memory block) is not accounted.
 * @param t root term on which used memory calculation will be performed.
 * @param max_stack_slots maximum number of stack items needed to process a term tree output parameter.
 * @param estimated_terms used memory count in term units output parameter.
 * @returns MEMORY_ESTIMATE_OK when successful.
 */
enum MemoryEstimateResult memory_estimate_term_memory_usage(term t, unsigned long *estimated_terms, int *max_stack_slots);

#endif
