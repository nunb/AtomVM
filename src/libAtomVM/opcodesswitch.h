/***************************************************************************
 *   Copyright 2017 by Davide Bettio <davide@uninstall.it>                 *
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

#include "module.h"

#include <assert.h>
#include <string.h>

#include "debug.h"
#include "exportedfunction.h"
#include "utils.h"
#include "scheduler.h"

#ifdef IMPL_EXECUTE_LOOP
    #include "mailbox.h"
#endif

//#define ENABLE_TRACE

#ifndef TRACE
    #ifdef ENABLE_TRACE
        #define TRACE printf
    #else
        #define TRACE(...)
    #endif
#endif

#define USED_BY_TRACE(x) \
    (void) (x)

#define COMPACT_SMALLINT4 1
#define COMPACT_ATOM 2
#define COMPACT_XREG 3
#define COMPACT_YREG 4
#define COMPACT_EXTENDED 7
#define COMPACT_LARGE_INTEGER 9
#define COMPACT_LARGE_ATOM 10

#define COMPACT_EXTENDED_LITERAL 0x47

#define COMPACT_LARGE_IMM_MASK 0x18
#define COMPACT_11BITS_VALUE 0x8
#define COMPACT_NBITS_VALUE 0x18

#ifdef IMPL_CODE_LOADER
#define DECODE_COMPACT_TERM(dest_term, code_chunk, base_index, off, next_operand_offset)\
{                                                                                       \
    uint8_t first_byte = (code_chunk[(base_index) + (off)]);                            \
    switch (first_byte & 0xF) {                                                         \
        case COMPACT_SMALLINT4:                                                         \
        case COMPACT_ATOM:                                                              \
        case COMPACT_XREG:                                                              \
        case COMPACT_YREG:                                                              \
            next_operand_offset += 1;                                                   \
            break;                                                                      \
                                                                                        \
        case COMPACT_EXTENDED:                                                          \
            switch (first_byte) {                                                       \
                case COMPACT_EXTENDED_LITERAL: {                                        \
                    uint8_t ext = (code_chunk[(base_index) + (off) + 1] & 0xF);         \
                    if (ext == 0) {                                                     \
                        next_operand_offset += 2;                                       \
                    }else if (ext == 0x8) {                                             \
                        next_operand_offset += 3;                                       \
                    } else {                                                            \
                        abort();                                                        \
                    }                                                                   \
                    break;                                                              \
                }                                                                       \
                default:                                                                \
                    printf("Unexpected %i\n", (int) first_byte);                        \
                    abort();                                                            \
                    break;                                                              \
            }                                                                           \
            break;                                                                      \
                                                                                        \
        case COMPACT_LARGE_INTEGER:                                                     \
        case COMPACT_LARGE_ATOM:                                                        \
            switch (first_byte & COMPACT_LARGE_IMM_MASK) {                              \
                case COMPACT_11BITS_VALUE:                                              \
                    next_operand_offset += 2;                                           \
                    break;                                                              \
                                                                                        \
                default:                                                                \
                    assert((first_byte & 0x30) != COMPACT_LARGE_INTEGER);               \
                    break;                                                              \
            }                                                                           \
            break;                                                                      \
                                                                                        \
        default:                                                                        \
            fprintf(stderr, "unknown compect term type: %i\n", ((first_byte) & 0xF));   \
            abort();                                                                    \
            break;                                                                      \
    }                                                                                   \
}
#endif

#ifdef IMPL_EXECUTE_LOOP
#define DECODE_COMPACT_TERM(dest_term, code_chunk, base_index, off, next_operand_offset)                                \
{                                                                                                                       \
    uint8_t first_byte = (code_chunk[(base_index) + (off)]);                                                            \
    switch (first_byte & 0xF) {                                                                                         \
        case COMPACT_SMALLINT4:                                                                                         \
            dest_term = term_from_int4(first_byte >> 4);                                                                \
            next_operand_offset += 1;                                                                                   \
            break;                                                                                                      \
                                                                                                                        \
        case COMPACT_ATOM:                                                                                              \
            if (first_byte == COMPACT_ATOM) {                                                                           \
                dest_term = term_nil();                                                                                 \
            } else {                                                                                                    \
                dest_term = module_get_atom_term_by_id(mod, first_byte >> 4);                                           \
            }                                                                                                           \
            next_operand_offset += 1;                                                                                   \
            break;                                                                                                      \
                                                                                                                        \
        case COMPACT_XREG:                                                                                              \
            dest_term = ctx->x[first_byte >> 4];                                                                        \
            next_operand_offset += 1;                                                                                   \
            break;                                                                                                      \
                                                                                                                        \
        case COMPACT_YREG:                                                                                              \
            dest_term = ctx->e[first_byte >> 4];                                                                        \
            next_operand_offset += 1;                                                                                   \
            break;                                                                                                      \
                                                                                                                        \
        case COMPACT_EXTENDED:                                                                                          \
            switch (first_byte) {                                                                                       \
                case COMPACT_EXTENDED_LITERAL: {                                                                        \
                    uint8_t first_extended_byte = code_chunk[(base_index) + (off) + 1];                                 \
                    if (!(first_extended_byte & 0xF)) {                                                                 \
                        dest_term = module_load_literal(mod, first_extended_byte >> 4, ctx);                            \
                        next_operand_offset += 2;                                                                       \
                    } else if ((first_extended_byte & 0xF) == 0x8) {                                                    \
                        uint8_t byte_1 = code_chunk[(base_index) + (off) + 2];                                          \
                        uint16_t index = (((uint16_t) first_extended_byte & 0xE0) << 3) | byte_1;                       \
                        dest_term = module_load_literal(mod, index, ctx);                                               \
                        next_operand_offset += 3;                                                                       \
                    } else {                                                                                            \
                        abort();                                                                                        \
                    }                                                                                                   \
                                                                                                                        \
                    break;                                                                                              \
                }                                                                                                       \
                default:                                                                                                \
                    abort();                                                                                            \
                    break;                                                                                              \
            }                                                                                                           \
            break;                                                                                                      \
                                                                                                                        \
        case COMPACT_LARGE_ATOM:                                                                                        \
            switch (first_byte & COMPACT_LARGE_IMM_MASK) {                                                              \
                case COMPACT_11BITS_VALUE:                                                                              \
                    dest_term = module_get_atom_term_by_id(mod, ((first_byte & 0xE0) << 3) | code_chunk[(base_index) + (off) + 1]); \
                    next_operand_offset += 2;                                                                           \
                    break;                                                                                              \
                                                                                                                        \
                default:                                                                                                \
                    abort();                                                                                            \
                    break;                                                                                              \
            }                                                                                                           \
            break;                                                                                                      \
                                                                                                                        \
        case COMPACT_LARGE_INTEGER:                                                                                     \
            switch (first_byte & COMPACT_LARGE_IMM_MASK) {                                                              \
                case COMPACT_11BITS_VALUE:                                                                              \
                    dest_term = term_from_int11(((first_byte & 0xE0) << 3) | code_chunk[(base_index) + (off) + 1]);     \
                    next_operand_offset += 2;                                                                           \
                    break;                                                                                              \
                                                                                                                        \
                default:                                                                                                \
                    abort();                                                                                            \
                    break;                                                                                              \
            }                                                                                                           \
            break;                                                                                                      \
                                                                                                                        \
        default:                                                                                                        \
            abort();                                                                                                    \
    }                                                                                                                   \
}
#endif


#define DECODE_LABEL(label, code_chunk, base_index, off, next_operand_offset)                       \
{                                                                                                   \
    uint8_t first_byte = (code_chunk[(base_index) + (off)]);                                        \
    switch (((first_byte) >> 3) & 0x3) {                                                            \
        case 0:                                                                                     \
        case 2:                                                                                     \
            label = first_byte >> 4;                                                                \
            next_operand_offset += 1;                                                               \
            break;                                                                                  \
                                                                                                    \
        case 1:                                                                                     \
            label = ((first_byte & 0xE0) << 3) | code_chunk[(base_index) + (off) + 1];              \
            next_operand_offset += 2;                                                               \
            break;                                                                                  \
                                                                                                    \
        default:                                                                                    \
            fprintf(stderr, "Operand not a label: %x, or unsupported encoding\n", (first_byte));    \
            abort();                                                                                \
            break;                                                                                  \
    }                                                                                               \
}

#define DECODE_ATOM(atom, code_chunk, base_index, off, next_operand_offset)                         \
{                                                                                                   \
    uint8_t first_byte = (code_chunk[(base_index) + (off)]);                                        \
    switch (((first_byte) >> 3) & 0x3) {                                                            \
        case 0:                                                                                     \
        case 2:                                                                                     \
            atom = first_byte >> 4;                                                                 \
            next_operand_offset += 1;                                                               \
            break;                                                                                  \
                                                                                                    \
        case 1:                                                                                     \
            atom = ((first_byte & 0xE0) << 3) | code_chunk[(base_index) + (off) + 1];               \
            next_operand_offset += 2;                                                               \
            break;                                                                                  \
                                                                                                    \
        default:                                                                                    \
            fprintf(stderr, "Operand not a label: %x, or unsupported encoding\n", (first_byte));    \
            abort();                                                                                \
            break;                                                                                  \
    }                                                                                               \
}

#define DECODE_INTEGER(label, code_chunk, base_index, off, next_operand_offset)                     \
{                                                                                                   \
    uint8_t first_byte = (code_chunk[(base_index) + (off)]);                                        \
    switch (((first_byte) >> 3) & 0x3) {                                                            \
        case 0:                                                                                     \
        case 2:                                                                                     \
            label = first_byte >> 4;                                                                \
            next_operand_offset += 1;                                                               \
            break;                                                                                  \
                                                                                                    \
        case 1:                                                                                     \
            label = ((first_byte & 0xE0) << 3) | code_chunk[(base_index) + (off) + 1];              \
            next_operand_offset += 2;                                                               \
            break;                                                                                  \
                                                                                                    \
        default:                                                                                    \
            fprintf(stderr, "Operand not an integer: %x, or unsupported encoding\n", (first_byte)); \
            abort();                                                                                \
            break;                                                                                  \
    }                                                                                               \
}

#define DECODE_DEST_REGISTER(dreg, dreg_type, code_chunk, base_index, off, next_operand_offset)     \
{                                                                                                   \
    dreg_type = code_chunk[(base_index) + (off)] & 0xF;                                             \
    dreg = code_chunk[(base_index) + (off)] >> 4;                                                   \
    next_operand_offset++;                                                                          \
}

#define WRITE_REGISTER(dreg_type, dreg, value)                                                      \
{                                                                                                   \
    switch (dreg_type) {                                                                            \
        case 3:                                                                                     \
            ctx->x[dreg] = value;                                                                   \
            break;                                                                                  \
        case 4:                                                                                     \
            ctx->e[dreg] = value;                                                                   \
            break;                                                                                  \
        default:                                                                                    \
            abort();                                                                                \
    }                                                                                               \
}


#define NEXT_INSTRUCTION(operands_size) \
    i += operands_size

#ifndef TRACE_JUMP
    #define JUMP_TO_ADDRESS(address) \
        i = ((uint8_t *) (address)) - code
#else
    #define JUMP_TO_ADDRESS(address) \
        i = ((uint8_t *) (address)) - code; \
        fprintf(stderr, "going to jump to %i\n", i)
#endif

#define SCHEDULE_NEXT(restore_mod, restore_to) \
    {                                                                                             \
        ctx->saved_ip = restore_to;                                                               \
        ctx->jump_to_on_restore = NULL;                                                           \
        ctx->saved_module = restore_mod;                                                          \
        Context *scheduled_context = scheduler_next(ctx->global, ctx);                            \
        ctx = scheduled_context;                                                                  \
        mod = ctx->saved_module;                                                                  \
        code = mod->code->code;                                                                   \
        remaining_reductions = DEFAULT_REDUCTIONS_AMOUNT;                                         \
        if (scheduled_context->jump_to_on_restore && ctx->mailbox) {                              \
            JUMP_TO_ADDRESS(scheduled_context->jump_to_on_restore);                               \
        } else {                                                                                  \
            JUMP_TO_ADDRESS(scheduled_context->saved_ip);                                         \
        }                                                                                         \
    }

#define OP_LABEL 1
#define OP_FUNC_INFO 2
#define OP_INT_CALL_END 3
#define OP_CALL 4
#define OP_CALL_LAST 5
#define OP_CALL_ONLY 6
#define OP_CALL_EXT 7
#define OP_CALL_EXT_LAST 8
#define OP_BIF0 9
#define OP_BIF1 10
#define OP_BIF2 11
#define OP_ALLOCATE 12
#define OP_ALLOCATE_HEAP 13
#define OP_ALLOCATE_ZERO 14
#define OP_TEST_HEAP 16
#define OP_KILL 17
#define OP_DEALLOCATE 18
#define OP_RETURN 19
#define OP_SEND 20
#define OP_REMOVE_MESSAGE 21
#define OP_TIMEOUT 22
#define OP_LOOP_REC 23
#define OP_WAIT 25
#define OP_WAIT_TIMEOUT 26
#define OP_IS_LT 39
#define OP_IS_GE 40
#define OP_IS_EQUAL 41
#define OP_IS_NOT_EQUAL 42
#define OP_IS_EQ_EXACT 43
#define OP_IS_NOT_EQ_EXACT 44
#define OP_IS_INTEGER 45
#define OP_IS_NUMBER 47
#define OP_IS_ATOM 48
#define OP_IS_PID 49
#define OP_IS_PORT 51
#define OP_IS_NIL 52
#define OP_IS_BINARY 53
#define OP_IS_LIST 55
#define OP_IS_NONEMPTY_LIST 56
#define OP_IS_TUPLE 57
#define OP_TEST_ARITY 58
#define OP_SELECT_VAL 59
#define OP_SELECT_TUPLE_ARITY 60
#define OP_JUMP 61
#define OP_MOVE 64
#define OP_GET_LIST 65
#define OP_GET_TUPLE_ELEMENT 66
#define OP_PUT_LIST 69
#define OP_PUT_TUPLE 70
#define OP_PUT 71
#define OP_BADMATCH 72
#define OP_IF_END 73
#define OP_CASE_END 74
#define OP_CALL_EXT_ONLY 78
#define OP_IS_BOOLEAN 114
#define OP_GC_BIF1 124
#define OP_GC_BIF2 125
#define OP_TRIM 136
#define OP_LINE 153

#define INSTRUCTION_POINTER() \
    ((const void *) &code[i])

#define DO_RETURN() \
    mod = mod->global->modules_by_index[ctx->cp >> 24]; \
    code = mod->code->code; \
    i = (ctx->cp & 0xFFFFFF) >> 2;

#define POINTER_TO_II(instruction_pointer) \
    (((uint8_t *) (instruction_pointer)) - code)

static const char *const true_atom = "\x04" "true";
static const char *const false_atom = "\x05" "false";

static inline term term_from_atom_string(GlobalContext *glb, AtomString string)
{
    int global_atom_index = globalcontext_insert_atom(glb, string);
    return term_from_atom_index(global_atom_index);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"

#ifdef IMPL_CODE_LOADER
    int read_core_chunk(Module *mod)
#else
    #ifdef IMPL_EXECUTE_LOOP
        HOT_FUNC int context_execute_loop(Context *ctx, Module *mod, const char *function_name, int arity)
    #else
        #error Need implementation type
    #endif
#endif
{
    uint8_t *code = mod->code->code;

    unsigned int i = 0;

    #ifdef IMPL_CODE_LOADER
        TRACE("-- Loading code\n");
    #endif

    #ifdef IMPL_EXECUTE_LOOP
        TRACE("-- Executing code\n");

        int function_len = strlen(function_name);
        uint8_t *tmp_atom_name = malloc(function_len + 1);
        tmp_atom_name[0] = function_len;
        memcpy(tmp_atom_name + 1, function_name, function_len);

        int label = module_search_exported_function(mod, tmp_atom_name, arity);
        free(tmp_atom_name);

        ctx->cp = module_address(mod->module_index, mod->end_instruction_ii);
        JUMP_TO_ADDRESS(mod->labels[label]);

        int remaining_reductions = DEFAULT_REDUCTIONS_AMOUNT;
    #endif

    while(1) {

        switch (code[i]) {
            case OP_LABEL: {
                int label;
                int next_offset = 1;
                DECODE_LABEL(label, code, i, next_offset, next_offset)

                TRACE("label/1 label=%i\n", label);
                USED_BY_TRACE(label);

                #ifdef IMPL_CODE_LOADER
                    TRACE("Mark label %i here at %i\n", label, i);
                    module_add_label(mod, label, &code[i]);
                #endif

                NEXT_INSTRUCTION(next_offset);
                break;
            }

            case OP_FUNC_INFO: {
                int next_offset = 1;
                int module_atom;
                DECODE_ATOM(module_atom, code, i, next_offset, next_offset)
                int function_name_atom;
                DECODE_ATOM(function_name_atom, code, i, next_offset, next_offset)
                int arity;
                DECODE_INTEGER(arity, code, i, next_offset, next_offset);

                TRACE("func_info/3 module_name_a=%i, function_name_a=%i, arity=%i\n", module_atom, function_name_atom, arity);
                USED_BY_TRACE(function_name_atom);
                USED_BY_TRACE(module_atom);
                USED_BY_TRACE(arity);

                NEXT_INSTRUCTION(next_offset);
                break;
            }

            case OP_INT_CALL_END: {
                TRACE("int_call_end!\n");

            #ifdef IMPL_CODE_LOADER
                TRACE("-- Code loading finished --\n");
                return i;
            #endif

            #ifdef IMPL_EXECUTE_LOOP
                TRACE("-- Code execution finished for %i--\n", ctx->process_id);
                if (schudule_processes_count(ctx->global) == 1) {
                    scheduler_terminate(ctx->global, ctx);
                    return 0;
                }

                TRACE("WARNING: some processes are still running.\n");

                Context *scheduled_context = scheduler_next(ctx->global, ctx);
                if (scheduled_context == ctx) {
                    TRACE(stderr, "There are no more runnable processes\n");
                    return 0;
                }

                scheduler_terminate(ctx->global, ctx);

                ctx = scheduled_context;
                mod = ctx->saved_module;
                code = mod->code->code;
                remaining_reductions = DEFAULT_REDUCTIONS_AMOUNT;
                if (scheduled_context->jump_to_on_restore && ctx->mailbox) {
                    JUMP_TO_ADDRESS(scheduled_context->jump_to_on_restore);
                } else {
                    JUMP_TO_ADDRESS(scheduled_context->saved_ip);
                }

                break;
            #endif
            }

            case OP_CALL: {
                int next_offset = 1;
                int arity;
                DECODE_INTEGER(arity, code, i, next_offset, next_offset);
                int label;
                DECODE_LABEL(label, code, i, next_offset, next_offset);

                TRACE("call/2, arity=%i, label=%i\n", arity, label);
                USED_BY_TRACE(arity);

                #ifdef IMPL_EXECUTE_LOOP
                    NEXT_INSTRUCTION(next_offset);
                    ctx->cp = module_address(mod->module_index, i);

                    remaining_reductions--;
                    if (LIKELY(remaining_reductions)) {
                        JUMP_TO_ADDRESS(mod->labels[label]);
                    } else {
                        SCHEDULE_NEXT(mod, mod->labels[label]);
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    NEXT_INSTRUCTION(next_offset);
                #endif

                break;
            }

            case OP_CALL_LAST: {
                int next_offset = 1;
                int arity;
                DECODE_INTEGER(arity, code, i, next_offset, next_offset);
                int label;
                DECODE_LABEL(label, code, i, next_offset, next_offset);
                int n_words;
                DECODE_INTEGER(n_words, code, i, next_offset, next_offset);

                TRACE("call_last/3, arity=%i, label=%i, dellocate=%i\n", arity, label, n_words);
                USED_BY_TRACE(arity);
                USED_BY_TRACE(label);
                USED_BY_TRACE(n_words);

                #ifdef IMPL_EXECUTE_LOOP
                    ctx->cp = ctx->e[n_words];
                    ctx->e += (n_words + 1);

                    DEBUG_DUMP_STACK(ctx);

                    remaining_reductions--;
                    if (LIKELY(remaining_reductions)) {
                        JUMP_TO_ADDRESS(mod->labels[label]);
                    } else {
                        SCHEDULE_NEXT(mod, mod->labels[label]);
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    NEXT_INSTRUCTION(next_offset);
                #endif

                break;
            }

            case OP_CALL_ONLY: {
                int next_off = 1;
                int arity;
                DECODE_INTEGER(arity, code, i, next_off, next_off);
                int label;
                DECODE_LABEL(label, code, i, next_off, next_off)

                TRACE("call_only/2, arity=%i, label=%i\n", arity, label);
                USED_BY_TRACE(arity);
                USED_BY_TRACE(label);

                #ifdef IMPL_EXECUTE_LOOP

                    NEXT_INSTRUCTION(next_off);
                    remaining_reductions--;
                    if (LIKELY(remaining_reductions)) {
                        JUMP_TO_ADDRESS(mod->labels[label]);
                    } else {
                        SCHEDULE_NEXT(mod, mod->labels[label]);
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    NEXT_INSTRUCTION(next_off);
                #endif

                break;
            }

            case OP_CALL_EXT: {
                int next_off = 1;
                int arity;
                DECODE_INTEGER(arity, code, i, next_off, next_off);
                int index;
                DECODE_INTEGER(index, code, i, next_off, next_off);

                TRACE("call_ext/2, arity=%i, index=%i\n", arity, index);
                USED_BY_TRACE(arity);
                USED_BY_TRACE(index);

                NEXT_INSTRUCTION(next_off);

                #ifdef IMPL_EXECUTE_LOOP
                    remaining_reductions--;
                    if (UNLIKELY(!remaining_reductions)) {
                        SCHEDULE_NEXT(mod, INSTRUCTION_POINTER());
                        continue;
                    }

                    const struct ExportedFunction *func = mod->imported_funcs[index].func;

                    if (func->type == UnresolvedFunctionCall) {
                        func = module_resolve_function(mod, index);
                    }

                    switch (func->type) {
                        case NIFFunctionType: {
                            const struct Nif *nif = EXPORTED_FUNCTION_TO_NIF(func);
                            ctx->x[0] = nif->nif_ptr(ctx, arity, ctx->x);
                            break;
                        }
                        case ModuleFunction: {
                            const struct ModuleFunction *jump = EXPORTED_FUNCTION_TO_MODULE_FUNCTION(func);

                            ctx->cp = module_address(mod->module_index, i);
                            mod = jump->target;
                            code = mod->code->code;
                            JUMP_TO_ADDRESS(mod->labels[jump->label]);

                            break;
                        }
                        default: {
                            fprintf(stderr, "Invalid function type %i at index: %i\n", func->type, index);
                            abort();
                        }
                    }
                #endif

                break;
            }

            case OP_CALL_EXT_LAST: {
                int next_off = 1;
                int arity;
                DECODE_INTEGER(arity, code, i, next_off, next_off);
                int index;
                DECODE_INTEGER(index, code, i, next_off, next_off);
                int n_words;
                DECODE_INTEGER(n_words, code, i, next_off, next_off);

                TRACE("call_ext_last/3, arity=%i, index=%i, n_words=%i\n", arity, index, n_words);
                USED_BY_TRACE(arity);
                USED_BY_TRACE(index);
                USED_BY_TRACE(n_words);

                #ifdef IMPL_EXECUTE_LOOP
                    remaining_reductions--;
                    if (UNLIKELY(!remaining_reductions)) {
                        SCHEDULE_NEXT(mod, INSTRUCTION_POINTER());
                        continue;
                    }

                    ctx->cp = ctx->e[n_words];
                    ctx->e += (n_words + 1);

                    const struct ExportedFunction *func = mod->imported_funcs[index].func;

                    if (func->type == UnresolvedFunctionCall) {
                        func = module_resolve_function(mod, index);
                    }

                    switch (func->type) {
                        case NIFFunctionType: {
                            const struct Nif *nif = EXPORTED_FUNCTION_TO_NIF(func);
                            ctx->x[0] = nif->nif_ptr(ctx, arity, ctx->x);
                            DO_RETURN();

                            break;
                        }
                        case ModuleFunction: {
                            const struct ModuleFunction *jump = EXPORTED_FUNCTION_TO_MODULE_FUNCTION(func);

                            mod = jump->target;
                            code = mod->code->code;
                            JUMP_TO_ADDRESS(mod->labels[jump->label]);

                            break;
                        }
                        default: {
                            fprintf(stderr, "Invalid function type %i at index: %i\n", func->type, index);
                            abort();
                        }
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    NEXT_INSTRUCTION(next_off);
                #endif

                break;
            }

            case OP_BIF0: {
                int next_off = 1;
                int bif;
                DECODE_INTEGER(bif, code, i, next_off, next_off);
                int dreg;
                uint8_t dreg_type;
                DECODE_DEST_REGISTER(dreg, dreg_type, code, i, next_off, next_off);

                TRACE("bif0/2 bif=%i, dreg=%i\n", bif, dreg);
                USED_BY_TRACE(bif);
                USED_BY_TRACE(dreg);

                #ifdef IMPL_EXECUTE_LOOP
                    BifImpl0 func = (BifImpl0) mod->imported_funcs[bif].bif;
                    term ret = func(ctx);

                    WRITE_REGISTER(dreg_type, dreg, ret);
                #endif

                NEXT_INSTRUCTION(next_off);
                break;
            }

            //TODO: implement me
            case OP_BIF1: {
                int next_off = 1;
                int fail_label;
                DECODE_LABEL(fail_label, code, i, next_off, next_off);
                int bif;
                DECODE_INTEGER(bif, code, i, next_off, next_off);
                term arg1;
                DECODE_COMPACT_TERM(arg1, code, i, next_off, next_off)
                int dreg;
                uint8_t dreg_type;
                DECODE_DEST_REGISTER(dreg, dreg_type, code, i, next_off, next_off);

                TRACE("bif1/2 bif=%i, fail=%i, dreg=%i\n", bif, fail_label, dreg);
                USED_BY_TRACE(bif);
                USED_BY_TRACE(dreg);

                #ifdef IMPL_CODE_LOADER
                    UNUSED(arg1);
                #endif

                #ifdef IMPL_EXECUTE_LOOP
                    BifImpl1 func = (BifImpl1) mod->imported_funcs[bif].bif;
                    term ret = func(ctx, fail_label, arg1);

                    WRITE_REGISTER(dreg_type, dreg, ret);
                #endif

                NEXT_INSTRUCTION(next_off);
                break;
            }

            //TODO: implement me
            case OP_BIF2: {
                int next_off = 1;
                int fail_label;
                DECODE_LABEL(fail_label, code, i, next_off, next_off);
                int bif;
                DECODE_INTEGER(bif, code, i, next_off, next_off);
                term arg1;
                DECODE_COMPACT_TERM(arg1, code, i, next_off, next_off)
                term arg2;
                DECODE_COMPACT_TERM(arg2, code, i, next_off, next_off)
                int dreg;
                uint8_t dreg_type;
                DECODE_DEST_REGISTER(dreg, dreg_type, code, i, next_off, next_off);

                TRACE("bif2/2 bif=%i, fail=%i, dreg=%i\n", bif, fail_label, dreg);
                USED_BY_TRACE(bif);
                USED_BY_TRACE(dreg);

                #ifdef IMPL_CODE_LOADER
                    UNUSED(arg1);
                    UNUSED(arg2);
                #endif

                #ifdef IMPL_EXECUTE_LOOP
                    BifImpl2 func = (BifImpl2) mod->imported_funcs[bif].bif;
                    term ret = func(ctx, fail_label, arg1, arg2);

                    WRITE_REGISTER(dreg_type, dreg, ret);
                #endif

                NEXT_INSTRUCTION(next_off);
                break;
            }

            case OP_ALLOCATE: {
                int next_off = 1;
                int stack_need;
                DECODE_INTEGER(stack_need, code, i, next_off, next_off);
                int live;
                DECODE_INTEGER(live, code, i, next_off, next_off);
                TRACE("allocate/2 stack_need=%i, live=%i\n" , stack_need, live);
                USED_BY_TRACE(stack_need);
                USED_BY_TRACE(live);

                #ifdef IMPL_EXECUTE_LOOP
                    if (live > ctx->avail_registers) {
                        fprintf(stderr, "Cannot use more than 16 registers.");
                        abort();
                    }

                    context_clean_registers(ctx, live);

                    if (ctx->heap_ptr > ctx->e - (stack_need + 1)) {
                        memory_ensure_free(ctx, stack_need + 1);
                    }
                    ctx->e -= stack_need + 1;
                    ctx->e[stack_need] = ctx->cp;
                #endif

                NEXT_INSTRUCTION(next_off);
                break;
            }

            case OP_ALLOCATE_HEAP: {
                int next_off = 1;
                int stack_need;
                DECODE_INTEGER(stack_need, code, i, next_off, next_off);
                int heap_need;
                DECODE_INTEGER(heap_need, code, i, next_off, next_off);
                int live;
                DECODE_INTEGER(live, code, i, next_off, next_off);
                TRACE("allocate_heap/2 stack_need=%i, heap_need=%i, live=%i\n", stack_need, heap_need, live);
                USED_BY_TRACE(stack_need);
                USED_BY_TRACE(heap_need);
                USED_BY_TRACE(live);

                #ifdef IMPL_EXECUTE_LOOP
                    if (live > ctx->avail_registers) {
                        fprintf(stderr, "Cannot use more than 16 registers.");
                        abort();
                    }

                    context_clean_registers(ctx, live);

                    if ((ctx->heap_ptr + heap_need) > ctx->e - (stack_need + 1)) {
                        memory_ensure_free(ctx, heap_need + stack_need + 1);
                    }
                    ctx->e -= stack_need + 1;
                    ctx->e[stack_need] = ctx->cp;
                #endif

                NEXT_INSTRUCTION(next_off);
                break;
            }

            case OP_ALLOCATE_ZERO: {
                int next_off = 1;
                int stack_need;
                DECODE_INTEGER(stack_need, code, i, next_off, next_off);
                int live;
                DECODE_INTEGER(live, code, i, next_off, next_off);
                TRACE("allocate_zero/2 stack_need=%i, live=%i\n", stack_need, live);
                USED_BY_TRACE(stack_need);
                USED_BY_TRACE(live);

                #ifdef IMPL_EXECUTE_LOOP
                    if (live > ctx->avail_registers) {
                        fprintf(stderr, "Cannot use more than 16 registers.");
                        abort();
                    }

                    context_clean_registers(ctx, live);

                    if (ctx->heap_ptr > ctx->e - (stack_need + 1)) {
                        memory_ensure_free(ctx, stack_need + 1);
                    }

                    ctx->e -= stack_need + 1;
                    for (int s = 0; s < stack_need; s++) {
                        ctx->e[s] = term_nil();
                    }
                    ctx->e[stack_need] = ctx->cp;
                #endif

                NEXT_INSTRUCTION(next_off);
                break;
            }

            case OP_TEST_HEAP: {
                int next_offset = 1;
                unsigned int heap_need;
                DECODE_INTEGER(heap_need, code, i, next_offset, next_offset);
                int live_registers;
                DECODE_INTEGER(live_registers, code, i, next_offset, next_offset);

                TRACE("test_heap/2 heap_need=%i, live_registers=%i\n", heap_need, live_registers);
                USED_BY_TRACE(heap_need);
                USED_BY_TRACE(live_registers);

                #ifdef IMPL_EXECUTE_LOOP
                    if (context_avail_free_memory(ctx) < heap_need) {
                        context_clean_registers(ctx, live_registers);
                        memory_ensure_free(ctx, heap_need);
                    } else if (context_avail_free_memory(ctx) > heap_need * HEAP_NEED_GC_SHRINK_THRESHOLD_COEFF) {
                        context_clean_registers(ctx, live_registers);
                        int used_size = context_memory_size(ctx) - context_avail_free_memory(ctx);
                        memory_ensure_free(ctx, used_size + heap_need * (HEAP_NEED_GC_SHRINK_THRESHOLD_COEFF / 2));
                    }
                #endif

                NEXT_INSTRUCTION(next_offset);
                break;
            }

            case OP_KILL: {
                int next_offset = 1;
                int target;
                DECODE_INTEGER(target, code, i, next_offset, next_offset);

                TRACE("kill/1 target=%i\n", target);

                #ifdef IMPL_EXECUTE_LOOP
                    ctx->e[target] = term_nil();
                #endif

                NEXT_INSTRUCTION(next_offset);

                break;
            }

            case OP_DEALLOCATE: {
                int next_off = 1;
                int n_words;
                DECODE_INTEGER(n_words, code, i, next_off, next_off);

                TRACE("deallocate/1 n_words=%i\n", n_words);
                USED_BY_TRACE(n_words);

                #ifdef IMPL_EXECUTE_LOOP
                    DEBUG_DUMP_STACK(ctx);

                    ctx->cp = ctx->e[n_words];
                    ctx->e += n_words + 1;
                    DEBUG_DUMP_STACK(ctx);
                #endif

                NEXT_INSTRUCTION(next_off);
                break;
            }

            case OP_RETURN: {
                TRACE("return/0\n");

                #ifdef IMPL_EXECUTE_LOOP
                    if ((long) ctx->cp == -1) {
                        return 0;
                    }

                    DO_RETURN();
                #endif

                #ifdef IMPL_CODE_LOADER
                    NEXT_INSTRUCTION(1);
                #endif
                break;
            }

            //TODO: implement send/0
            case OP_SEND: {
                #ifdef IMPL_CODE_LOADER
                    TRACE("send/0\n");
                #endif

                #ifdef IMPL_EXECUTE_LOOP
                    int local_process_id = term_to_local_process_id(ctx->x[0]);
                    TRACE("send/0 target_pid=%i\n", local_process_id);
                    Context *target = globalcontext_get_process(ctx->global, local_process_id);

                    mailbox_send(target, ctx->x[1]);
                #endif

                NEXT_INSTRUCTION(1);
                break;
            }

            //TODO: implement remove_message/0
            case OP_REMOVE_MESSAGE: {
                TRACE("remove_message/0\n");

                #ifdef IMPL_EXECUTE_LOOP
                    ctx->x[0] = mailbox_receive(ctx);
                #endif

                NEXT_INSTRUCTION(1);
                break;
            }

            //TODO: implement timeout/0
            case OP_TIMEOUT: {
                TRACE("timeout/0\n");

                NEXT_INSTRUCTION(1);
                break;
            }

            //TODO: implemente loop_rec/2
            case OP_LOOP_REC: {
                int next_off = 1;
                int label;
                DECODE_LABEL(label, code, i, next_off, next_off)
                int dreg;
                uint8_t dreg_type;
                DECODE_DEST_REGISTER(dreg, dreg_type, code, i, next_off, next_off);

                TRACE("loop_rec/2, dreg=%i\n", dreg);
                USED_BY_TRACE(dreg);

                #ifdef IMPL_EXECUTE_LOOP
                    if (ctx->mailbox == NULL) {
                        JUMP_TO_ADDRESS(mod->labels[label]);
                    } else {
                        term ret = mailbox_peek(ctx);

                        WRITE_REGISTER(dreg_type, dreg, ret);
                        NEXT_INSTRUCTION(next_off);
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    NEXT_INSTRUCTION(next_off);
                #endif

                break;
            }

            //TODO: implement wait/1
            case OP_WAIT: {
                int next_off = 1;
                int label;
                DECODE_LABEL(label, code, i, next_off, next_off)

                TRACE("wait/1\n");

                #ifdef IMPL_EXECUTE_LOOP
                    ctx->saved_ip = mod->labels[label];
                    ctx->jump_to_on_restore = NULL;
                    ctx->saved_module = mod;
                    Context *scheduled_context = scheduler_wait(ctx->global, ctx, -1);
                    ctx = scheduled_context;

                    mod = ctx->saved_module;
                    code = mod->code->code;
                    if (scheduled_context->jump_to_on_restore && ctx->mailbox) {
                        JUMP_TO_ADDRESS(scheduled_context->jump_to_on_restore);
                    } else {
                        JUMP_TO_ADDRESS(scheduled_context->saved_ip);
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    NEXT_INSTRUCTION(next_off);
                #endif

                break;
            }

            //TODO: implement wait_timeout/2
            case OP_WAIT_TIMEOUT: {
                int next_off = 1;
                int label;
                DECODE_LABEL(label, code, i, next_off, next_off)
                int timeout;
                DECODE_COMPACT_TERM(timeout, code, i, next_off, next_off)

                #ifdef IMPL_EXECUTE_LOOP
                    TRACE("wait_timeout/2, label: %i, timeout: %x\n", label, term_to_int32(timeout));

                    NEXT_INSTRUCTION(next_off);
                    //TODO: it looks like x[0] might be used instead of jump_to_on_restore
                    ctx->saved_ip = INSTRUCTION_POINTER();
                    ctx->jump_to_on_restore = mod->labels[label];
                    ctx->saved_module = mod;
                    Context *scheduled_context = scheduler_wait(ctx->global, ctx, term_to_int32(timeout));
                    ctx = scheduled_context;

                    mod = ctx->saved_module;
                    code = mod->code->code;
                    if (scheduled_context->jump_to_on_restore && ctx->mailbox) {
                        JUMP_TO_ADDRESS(scheduled_context->jump_to_on_restore);
                    } else {
                        JUMP_TO_ADDRESS(scheduled_context->saved_ip);
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    TRACE("wait_timeout/2, label: %i\n", label);

                    UNUSED(timeout)

                    NEXT_INSTRUCTION(next_off);
                #endif

                break;
            }


            case OP_IS_LT: {
                int next_off = 1;
                int label;
                DECODE_LABEL(label, code, i, next_off, next_off);
                term arg1;
                DECODE_COMPACT_TERM(arg1, code, i, next_off, next_off);
                term arg2;
                DECODE_COMPACT_TERM(arg2, code, i, next_off, next_off);

                #ifdef IMPL_EXECUTE_LOOP
                    TRACE("is_lt/2, label=%i, arg1=%lx, arg2=%lx\n", label, arg1, arg2);

                    if (arg1 < arg2) {
                        NEXT_INSTRUCTION(next_off);
                    } else {
                        i = POINTER_TO_II(mod->labels[label]);
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    TRACE("is_lt/2\n");
                    UNUSED(arg1)
                    UNUSED(arg2)
                    NEXT_INSTRUCTION(next_off);
                #endif

                break;
            }

            case OP_IS_GE: {
                int next_off = 1;
                int label;
                DECODE_LABEL(label, code, i, next_off, next_off);
                term arg1;
                DECODE_COMPACT_TERM(arg1, code, i, next_off, next_off);
                term arg2;
                DECODE_COMPACT_TERM(arg2, code, i, next_off, next_off);

                #ifdef IMPL_EXECUTE_LOOP
                    TRACE("is_ge/2, label=%i, arg1=%lx, arg2=%lx\n", label, arg1, arg2);

                    if (arg1 >= arg2) {
                        NEXT_INSTRUCTION(next_off);
                    } else {
                        i = POINTER_TO_II(mod->labels[label]);
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    TRACE("is_ge/2\n");
                    UNUSED(arg1)
                    UNUSED(arg2)
                    NEXT_INSTRUCTION(next_off);
                #endif

                break;
            }

            case OP_IS_EQUAL: {
                int label;
                term arg1;
                term arg2;
                int next_off = 1;
                DECODE_LABEL(label, code, i, next_off, next_off)
                DECODE_COMPACT_TERM(arg1, code, i, next_off, next_off)
                DECODE_COMPACT_TERM(arg2, code, i, next_off, next_off)

                #ifdef IMPL_EXECUTE_LOOP
                    TRACE("is_equal/2, label=%i, arg1=%lx, arg2=%lx\n", label, arg1, arg2);

                    //TODO: implement this
                    if (arg1 == arg2) {
                        NEXT_INSTRUCTION(next_off);
                    } else {
                        i = POINTER_TO_II(mod->labels[label]);
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    TRACE("is_equal/2\n");
                    UNUSED(arg1)
                    UNUSED(arg2)
                    NEXT_INSTRUCTION(next_off);
                #endif

                break;
            }

            case OP_IS_NOT_EQUAL: {
                int next_off = 1;
                int label;
                DECODE_LABEL(label, code, i, next_off, next_off)
                term arg1;
                DECODE_COMPACT_TERM(arg1, code, i, next_off, next_off)
                term arg2;
                DECODE_COMPACT_TERM(arg2, code, i, next_off, next_off)

                #ifdef IMPL_EXECUTE_LOOP
                    TRACE("is_not_equall/2, label=%i, arg1=%lx, arg2=%lx\n", label, arg1, arg2);

                    if (arg1 != arg2) {
                        NEXT_INSTRUCTION(next_off);
                    } else {
                        i = POINTER_TO_II(mod->labels[label]);
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    TRACE("is_not_equal/2\n");
                    UNUSED(arg1)
                    UNUSED(arg2)
                    NEXT_INSTRUCTION(next_off);
                #endif

                break;
            }

            case OP_IS_EQ_EXACT: {
                int label;
                term arg1;
                term arg2;
                int next_off = 1;
                DECODE_LABEL(label, code, i, next_off, next_off)
                DECODE_COMPACT_TERM(arg1, code, i, next_off, next_off)
                DECODE_COMPACT_TERM(arg2, code, i, next_off, next_off)

                #ifdef IMPL_EXECUTE_LOOP
                    TRACE("is_eq_exact/2, label=%i, arg1=%lx, arg2=%lx\n", label, arg1, arg2);

                    //TODO: implement this
                    if (arg1 == arg2) {
                        NEXT_INSTRUCTION(next_off);
                    } else {
                        i = POINTER_TO_II(mod->labels[label]);
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    TRACE("is_eq_exact/2\n");
                    UNUSED(arg1)
                    UNUSED(arg2)
                    NEXT_INSTRUCTION(next_off);
                #endif

                break;
            }

            case OP_IS_NOT_EQ_EXACT: {
                int next_off = 1;
                int label;
                DECODE_LABEL(label, code, i, next_off, next_off)
                term arg1;
                DECODE_COMPACT_TERM(arg1, code, i, next_off, next_off)
                term arg2;
                DECODE_COMPACT_TERM(arg2, code, i, next_off, next_off)

                #ifdef IMPL_EXECUTE_LOOP
                    TRACE("is_not_eq_exact/2, label=%i, arg1=%lx, arg2=%lx\n", label, arg1, arg2);

                    //TODO: implement this
                    if (arg1 != arg2) {
                        NEXT_INSTRUCTION(next_off);
                    } else {
                        i = POINTER_TO_II(mod->labels[label]);
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    TRACE("is_not_eq_exact/2\n");
                    UNUSED(arg1)
                    UNUSED(arg2)
                    NEXT_INSTRUCTION(next_off);
                #endif

                break;
           }

           case OP_IS_INTEGER: {
                int next_off = 1;
                int label;
                DECODE_LABEL(label, code, i, next_off, next_off)
                term arg1;
                DECODE_COMPACT_TERM(arg1, code, i, next_off, next_off)

                #ifdef IMPL_EXECUTE_LOOP
                    TRACE("is_integer/2, label=%i, arg1=%lx\n", label, arg1);

                    if (term_is_integer(arg1)) {
                        NEXT_INSTRUCTION(next_off);
                    } else {
                        i = POINTER_TO_II(mod->labels[label]);
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    TRACE("is_integer/2\n");
                    UNUSED(label)
                    UNUSED(arg1)
                    NEXT_INSTRUCTION(next_off);
                #endif

                break;
            }

           case OP_IS_NUMBER: {
                int next_off = 1;
                int label;
                DECODE_LABEL(label, code, i, next_off, next_off)
                term arg1;
                DECODE_COMPACT_TERM(arg1, code, i, next_off, next_off)

                #ifdef IMPL_EXECUTE_LOOP
                    TRACE("is_number/2, label=%i, arg1=%lx\n", label, arg1);

                    //TODO: check for floats too
                    if (term_is_integer(arg1)) {
                        NEXT_INSTRUCTION(next_off);
                    } else {
                        i = POINTER_TO_II(mod->labels[label]);
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    TRACE("is_number/2\n");
                    UNUSED(label)
                    UNUSED(arg1)
                    NEXT_INSTRUCTION(next_off);
                #endif

                break;
            }

            case OP_IS_BINARY: {
                int next_off = 1;
                int label;
                DECODE_LABEL(label, code, i, next_off, next_off)
                term arg1;
                DECODE_COMPACT_TERM(arg1, code, i, next_off, next_off)

                #ifdef IMPL_EXECUTE_LOOP
                    TRACE("is_binary/2, label=%i, arg1=%lx\n", label, arg1);

                    if (term_is_binary(arg1) || term_is_nil(arg1)) {
                        NEXT_INSTRUCTION(next_off);
                    } else {
                        i = POINTER_TO_II(mod->labels[label]);
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    TRACE("is_binary/2\n");
                    UNUSED(arg1)
                    NEXT_INSTRUCTION(next_off);
                #endif

                break;
            }

            case OP_IS_LIST: {
                int next_off = 1;
                int label;
                DECODE_LABEL(label, code, i, next_off, next_off)
                term arg1;
                DECODE_COMPACT_TERM(arg1, code, i, next_off, next_off)

                #ifdef IMPL_EXECUTE_LOOP
                    TRACE("is_list/2, label=%i, arg1=%lx\n", label, arg1);

                    if (term_is_list(arg1) || term_is_nil(arg1)) {
                        NEXT_INSTRUCTION(next_off);
                    } else {
                        i = POINTER_TO_II(mod->labels[label]);
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    TRACE("is_list/2\n");
                    UNUSED(arg1)
                    NEXT_INSTRUCTION(next_off);
                #endif

                break;
            }

            case OP_IS_NONEMPTY_LIST: {
                int label;
                term arg1;
                int next_off = 1;
                DECODE_LABEL(label, code, i, next_off, next_off)
                DECODE_COMPACT_TERM(arg1, code, i, next_off, next_off)

                #ifdef IMPL_EXECUTE_LOOP
                    TRACE("is_nonempty_list/2, label=%i, arg1=%lx\n", label, arg1);

                    if (term_is_list(arg1)) {
                        NEXT_INSTRUCTION(next_off);
                    } else {
                        i = POINTER_TO_II(mod->labels[label]);
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    TRACE("is_nonempty_list/2\n");
                    UNUSED(arg1)
                    NEXT_INSTRUCTION(next_off);
                #endif

                break;
            }

            case OP_IS_NIL: {
                int label;
                term arg1;
                int next_off = 1;
                DECODE_LABEL(label, code, i, next_off, next_off)
                DECODE_COMPACT_TERM(arg1, code, i, next_off, next_off)

                #ifdef IMPL_EXECUTE_LOOP
                    TRACE("is_nil/2, label=%i, arg1=%lx\n", label, arg1);

                    if (term_is_nil(arg1)) {
                        NEXT_INSTRUCTION(next_off);
                    } else {
                        i = POINTER_TO_II(mod->labels[label]);
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    TRACE("is_nil/2\n");
                    UNUSED(arg1)
                    NEXT_INSTRUCTION(next_off);
                #endif

                break;
            }

            case OP_IS_ATOM: {
                int label;
                term arg1;
                int next_off = 1;
                DECODE_LABEL(label, code, i, next_off, next_off)
                DECODE_COMPACT_TERM(arg1, code, i, next_off, next_off)

                #ifdef IMPL_EXECUTE_LOOP
                    TRACE("is_atom/2, label=%i, arg1=%lx\n", label, arg1);

                    if (term_is_atom(arg1)) {
                        NEXT_INSTRUCTION(next_off);
                    } else {
                        i = POINTER_TO_II(mod->labels[label]);
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    TRACE("is_atom/2\n");
                    UNUSED(arg1)
                    NEXT_INSTRUCTION(next_off);
                #endif

                break;
            }

            case OP_IS_PID: {
                int next_off = 1;
                int label;
                DECODE_LABEL(label, code, i, next_off, next_off)
                term arg1;
                DECODE_COMPACT_TERM(arg1, code, i, next_off, next_off)

                #ifdef IMPL_EXECUTE_LOOP
                    TRACE("is_pid/2, label=%i, arg1=%lx\n", label, arg1);

                    if (term_is_pid(arg1)) {
                        NEXT_INSTRUCTION(next_off);
                    } else {
                        i = POINTER_TO_II(mod->labels[label]);
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    TRACE("is_pid/2\n");
                    UNUSED(arg1)
                    NEXT_INSTRUCTION(next_off);
                #endif

                break;
           }

            case OP_IS_PORT: {
                int next_off = 1;
                int label;
                DECODE_LABEL(label, code, i, next_off, next_off)
                term arg1;
                DECODE_COMPACT_TERM(arg1, code, i, next_off, next_off)

                #ifdef IMPL_EXECUTE_LOOP
                    TRACE("is_port/2, label=%i, arg1=%lx\n", label, arg1);

                    if (term_is_pid(arg1)) {
                        int local_process_id = term_to_local_process_id(arg1);
                        Context *target = globalcontext_get_process(ctx->global, local_process_id);

                        if (context_is_port_driver(target)) {
                            NEXT_INSTRUCTION(next_off);
                        } else {
                            i = POINTER_TO_II(mod->labels[label]);
                        }
                    } else {
                        i = POINTER_TO_II(mod->labels[label]);
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    TRACE("is_port/2\n");
                    UNUSED(arg1)
                    NEXT_INSTRUCTION(next_off);
                #endif

                break;
           }

           case OP_IS_TUPLE: {
                int next_off = 1;
                int label;
                DECODE_LABEL(label, code, i, next_off, next_off)
                term arg1;
                DECODE_COMPACT_TERM(arg1, code, i, next_off, next_off)

                #ifdef IMPL_EXECUTE_LOOP
                    TRACE("is_tuple/2, label=%i, arg1=%lx\n", label, arg1);

                    if (term_is_tuple(arg1)) {
                        NEXT_INSTRUCTION(next_off);
                    } else {
                        i = POINTER_TO_II(mod->labels[label]);
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    TRACE("is_tuple/2\n");
                    UNUSED(label)
                    UNUSED(arg1)
                    NEXT_INSTRUCTION(next_off);
                #endif

                break;
            }

           case OP_TEST_ARITY: {
                int next_off = 1;
                int label;
                DECODE_LABEL(label, code, i, next_off, next_off);
                term arg1;
                DECODE_COMPACT_TERM(arg1, code, i, next_off, next_off);
                int arity;
                DECODE_INTEGER(arity, code, i, next_off, next_off);

                #ifdef IMPL_EXECUTE_LOOP
                    TRACE("test_arity/2, label=%i, arg1=%lx\n", label, arg1);

                    if (term_get_tuple_arity(arg1) == arity) {
                        NEXT_INSTRUCTION(next_off);
                    } else {
                        i = (uint8_t *) mod->labels[label] - code;
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    TRACE("test_arity/2\n");
                    UNUSED(label)
                    UNUSED(arg1)
                    NEXT_INSTRUCTION(next_off);
                #endif

                break;
            }

            case OP_SELECT_VAL: {
                int next_off = 1;
                term src_value;
                DECODE_COMPACT_TERM(src_value, code, i, next_off, next_off)
                int default_label;
                DECODE_LABEL(default_label, code, i, next_off, next_off)
                next_off++; //skip extended list tag
                int size;
                DECODE_INTEGER(size, code, i, next_off, next_off)

                TRACE("select_val/3, default_label=%i, vals=%i\n", default_label, size);
                USED_BY_TRACE(default_label);
                USED_BY_TRACE(size);

                #ifdef IMPL_CODE_LOADER
                    UNUSED(src_value);
                #endif

                #ifdef IMPL_EXECUTE_LOOP
                    void *jump_to_address = NULL;
                #endif

                for (int j = 0; j < size / 2; j++) {
                    term cmp_value;
                    DECODE_COMPACT_TERM(cmp_value, code, i, next_off, next_off)
                    int jmp_label;
                    DECODE_LABEL(jmp_label, code, i, next_off, next_off)

                    #ifdef IMPL_CODE_LOADER
                        UNUSED(cmp_value);
                    #endif

                    #ifdef IMPL_EXECUTE_LOOP
                        if (!jump_to_address && (src_value == cmp_value)) {
                            jump_to_address = mod->labels[jmp_label];
                        }
                    #endif
                }

                #ifdef IMPL_EXECUTE_LOOP
                    if (!jump_to_address) {
                        JUMP_TO_ADDRESS(mod->labels[default_label]);
                    } else {
                        JUMP_TO_ADDRESS(jump_to_address);
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    NEXT_INSTRUCTION(next_off);
                #endif

                break;
            }

            case OP_SELECT_TUPLE_ARITY: {
                int next_off = 1;
                term src_value;
                DECODE_INTEGER(src_value, code, i, next_off, next_off)
                int default_label;
                DECODE_LABEL(default_label, code, i, next_off, next_off)
                next_off++; //skip extended list tag
                int size;
                DECODE_INTEGER(size, code, i, next_off, next_off)

                TRACE("select_tuple_arity/3, default_label=%i, vals=%i\n", default_label, size);
                USED_BY_TRACE(default_label);
                USED_BY_TRACE(size);

                #ifdef IMPL_CODE_LOADER
                    UNUSED(src_value);
                #endif

                #ifdef IMPL_EXECUTE_LOOP
                    void *jump_to_address = NULL;
                #endif

                for (int j = 0; j < size / 2; j++) {
                    int cmp_value;
                    DECODE_INTEGER(cmp_value, code, i, next_off, next_off)
                    int jmp_label;
                    DECODE_LABEL(jmp_label, code, i, next_off, next_off)

                    #ifdef IMPL_CODE_LOADER
                        UNUSED(cmp_value);
                    #endif

                    #ifdef IMPL_EXECUTE_LOOP
                        if (!jump_to_address && (term_get_tuple_arity(src_value) == cmp_value)) {
                            jump_to_address = mod->labels[jmp_label];
                        }
                    #endif
                }

                #ifdef IMPL_EXECUTE_LOOP
                    if (!jump_to_address) {
                        JUMP_TO_ADDRESS(mod->labels[default_label]);
                    } else {
                        JUMP_TO_ADDRESS(jump_to_address);
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    NEXT_INSTRUCTION(next_off);
                #endif

                break;
            }

            case OP_JUMP: {
                int label;
                int next_offset = 1;
                DECODE_LABEL(label, code, i, next_offset, next_offset)

                TRACE("jump/1 label=%i\n", label);
                USED_BY_TRACE(label);

                #ifdef IMPL_EXECUTE_LOOP
                    remaining_reductions--;
                    if (LIKELY(remaining_reductions)) {
                        JUMP_TO_ADDRESS(mod->labels[label]);
                    } else {
                        SCHEDULE_NEXT(mod, mod->labels[label]);
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    NEXT_INSTRUCTION(next_offset);
                #endif

                break;
            }

            case OP_MOVE: {
                int next_off = 1;
                term src_value;
                DECODE_COMPACT_TERM(src_value, code, i, next_off, next_off);
                int dreg;
                uint8_t dreg_type;
                DECODE_DEST_REGISTER(dreg, dreg_type, code, i, next_off, next_off);

                #ifdef IMPL_EXECUTE_LOOP
                    TRACE("move/2 %lx, %c%i\n", src_value, reg_type_c(dreg_type), dreg);

                    WRITE_REGISTER(dreg_type, dreg, src_value);
                #endif

                #ifdef IMPL_CODE_LOADER
                    TRACE("move/2\n");
                    UNUSED(src_value)
                #endif

                NEXT_INSTRUCTION(next_off);
                break;
            }

            case OP_GET_LIST: {
                int next_off = 1;
                term src_value;
                DECODE_COMPACT_TERM(src_value, code, i, next_off, next_off)
                int head_dreg;
                uint8_t head_dreg_type;
                DECODE_DEST_REGISTER(head_dreg, head_dreg_type, code, i, next_off, next_off);
                int tail_dreg;
                uint8_t tail_dreg_type;
                DECODE_DEST_REGISTER(tail_dreg, tail_dreg_type, code, i, next_off, next_off);

                #ifdef IMPL_EXECUTE_LOOP
                    TRACE("get_list/3 %lx, %c%i, %c%i\n", src_value, reg_type_c(head_dreg_type), head_dreg, reg_type_c(tail_dreg_type), tail_dreg);

                    term head = term_get_list_head(src_value);
                    term tail = term_get_list_tail(src_value);

                    WRITE_REGISTER(head_dreg_type, head_dreg, head);
                    WRITE_REGISTER(tail_dreg_type, tail_dreg, tail);
                #endif

                #ifdef IMPL_CODE_LOADER
                    TRACE("get_list/2\n");
                    UNUSED(src_value)
                #endif

                NEXT_INSTRUCTION(next_off);
                break;
            }

            case OP_GET_TUPLE_ELEMENT: {
                int next_off = 1;
                term src_value;
                DECODE_COMPACT_TERM(src_value, code, i, next_off, next_off);
                int element;
                DECODE_INTEGER(element, code, i, next_off, next_off);
                int dreg;
                uint8_t dreg_type;
                DECODE_DEST_REGISTER(dreg, dreg_type, code, i, next_off, next_off);

                TRACE("get_tuple_element/2, element=%i, dest=%c%i\n", element, reg_type_c(dreg_type), dreg);
                USED_BY_TRACE(element);

                #ifdef IMPL_EXECUTE_LOOP
                    term t = term_get_tuple_element(src_value, element);
                    WRITE_REGISTER(dreg_type, dreg, t);
                #endif

                #ifdef IMPL_CODE_LOADER
                    UNUSED(src_value)
                #endif

                NEXT_INSTRUCTION(next_off);
                break;
            }

            case OP_PUT_LIST: {
                #ifdef IMPL_EXECUTE_LOOP
                    term *list_elem = term_list_alloc(ctx);
                #endif

                int next_off = 1;
                term head;
                DECODE_COMPACT_TERM(head, code, i, next_off, next_off);
                term tail;
                DECODE_COMPACT_TERM(tail, code, i, next_off, next_off);
                int dreg;
                uint8_t dreg_type;
                DECODE_DEST_REGISTER(dreg, dreg_type, code, i, next_off, next_off);

                TRACE("op_put_list/3\n");

                #ifdef IMPL_CODE_LOADER
                    UNUSED(head);
                    UNUSED(tail);
                #endif

                #ifdef IMPL_EXECUTE_LOOP
                    term t = term_list_init_prepend(list_elem, head, tail);
                    WRITE_REGISTER(dreg_type, dreg, t);
                #endif

                NEXT_INSTRUCTION(next_off);
                break;
            }

            case OP_PUT_TUPLE: {
                int next_off = 1;
                int size;
                DECODE_INTEGER(size, code, i, next_off, next_off);
                int dreg;
                uint8_t dreg_type;
                DECODE_DEST_REGISTER(dreg, dreg_type, code, i, next_off, next_off);

                TRACE("put_tuple/2 size=%i, dest=%c%i\n", size, reg_type_c(dreg_type), dreg);
                USED_BY_TRACE(dreg);

                #ifdef IMPL_EXECUTE_LOOP
                    term t = term_alloc_tuple(size, ctx);
                    WRITE_REGISTER(dreg_type, dreg, t);
                #endif

                for (int j = 0; j < size; j++) {
                    if (code[i + next_off] != OP_PUT) {
                        fprintf(stderr, "Expected put, got opcode: %i\n", code[i + next_off]);
                        abort();
                    }
                    next_off++;
                    term put_value;
                    DECODE_COMPACT_TERM(put_value, code, i, next_off, next_off);
                    #ifdef IMPL_CODE_LOADER
                        TRACE("put/2\n");
                        UNUSED(put_value);
                    #endif

                    #ifdef IMPL_EXECUTE_LOOP
                        TRACE("put/2 elem=%i, value=0x%lx\n", j, put_value);
                        term_put_tuple_element(t, j, put_value);
                    #endif
                }

                NEXT_INSTRUCTION(next_off);
                break;
            }

            //TODO: implement
            case OP_BADMATCH: {
                int next_off = 1;
                term arg1;
                DECODE_COMPACT_TERM(arg1, code, i, next_off, next_off)

                #ifdef IMPL_CODE_LOADER
                    TRACE("badmatch/1\n");
                    USED_BY_TRACE(arg1);
                #endif

                #ifdef IMPL_EXECUTE_LOOP
                    TRACE("badmatch/1, v=0x%lx\n", arg1);
                    abort();
                #endif

                NEXT_INSTRUCTION(next_off);
                break;
            }

            //TODO: implement
            case OP_IF_END: {
                TRACE("if_end/0\n");

                #ifdef IMPL_EXECUTE_LOOP
                    abort();
                #endif

                NEXT_INSTRUCTION(1);
                break;
            }

            //TODO: implement
            case OP_CASE_END: {
                int next_off = 1;
                term arg1;
                DECODE_COMPACT_TERM(arg1, code, i, next_off, next_off)

                #ifdef IMPL_CODE_LOADER
                    TRACE("case_end/1\n");
                    USED_BY_TRACE(arg1);
                #endif

                #ifdef IMPL_EXECUTE_LOOP
                    TRACE("case_end/1, v=0x%lx\n", arg1);
                    abort();
                #endif

                NEXT_INSTRUCTION(next_off);
                break;
            }

            case OP_CALL_EXT_ONLY: {
                int next_off = 1;
                int arity;
                DECODE_INTEGER(arity, code, i, next_off, next_off);
                int index;
                DECODE_INTEGER(index, code, i, next_off, next_off);

                TRACE("call_ext_only/2, arity=%i, index=%i\n", arity, index);
                USED_BY_TRACE(arity);
                USED_BY_TRACE(index);

                #ifdef IMPL_CODE_LOADER
                    NEXT_INSTRUCTION(next_off);
                #endif

                #ifdef IMPL_EXECUTE_LOOP
                    remaining_reductions--;
                    if (UNLIKELY(!remaining_reductions)) {
                        SCHEDULE_NEXT(mod, INSTRUCTION_POINTER());
                        continue;
                    }

                    const struct ExportedFunction *func = mod->imported_funcs[index].func;

                    if (func->type == UnresolvedFunctionCall) {
                        func = module_resolve_function(mod, index);
                    }

                    switch (func->type) {
                        case NIFFunctionType: {
                            const struct Nif *nif = EXPORTED_FUNCTION_TO_NIF(func);
                            ctx->x[0] = nif->nif_ptr(ctx, arity, ctx->x);
                            if ((long) ctx->cp == -1) {
                                return 0;
                            }

                            DO_RETURN();

                            break;
                        }
                        case ModuleFunction: {
                            const struct ModuleFunction *jump = EXPORTED_FUNCTION_TO_MODULE_FUNCTION(func);

                            mod = jump->target;
                            code = mod->code->code;

                            JUMP_TO_ADDRESS(mod->labels[jump->label]);

                            break;
                        }
                        default: {
                            abort();
                        }
                    }
                #endif

                break;
            }

           case OP_IS_BOOLEAN: {
                int next_off = 1;
                int label;
                DECODE_LABEL(label, code, i, next_off, next_off)
                term arg1;
                DECODE_COMPACT_TERM(arg1, code, i, next_off, next_off)

                #ifdef IMPL_EXECUTE_LOOP
                    TRACE("is_boolean/2, label=%i, arg1=%lx\n", label, arg1);

                    static const char *const true_atom = "\x04" "true";
                    static const char *const false_atom = "\x05" "false";

                    term true_term = term_from_atom_string(ctx->global, true_atom);
                    term false_term = term_from_atom_string(ctx->global, false_atom);

                    if ((arg1 == true_term) || (arg1 == false_term)) {
                        NEXT_INSTRUCTION(next_off);
                    } else {
                        i = POINTER_TO_II(mod->labels[label]);
                    }
                #endif

                #ifdef IMPL_CODE_LOADER
                    TRACE("is_boolean/2\n");
                    UNUSED(label)
                    UNUSED(arg1)
                    NEXT_INSTRUCTION(next_off);
                #endif

                break;
            }

            case OP_GC_BIF1: {
                int next_off = 1;
                int f_label;
                DECODE_LABEL(f_label, code, i, next_off, next_off);
                int live;
                DECODE_INTEGER(live, code, i, next_off, next_off);
                int bif;
                DECODE_INTEGER(bif, code, i, next_off, next_off); //s?
                term arg1;
                DECODE_COMPACT_TERM(arg1, code, i, next_off, next_off)
                int dreg;
                uint8_t dreg_type;
                DECODE_DEST_REGISTER(dreg, dreg_type, code, i, next_off, next_off);

                #ifdef IMPL_EXECUTE_LOOP
                    TRACE("gc_bif1/5 fail_lbl=%i, live=%i, bif=%i, arg1=0x%lx, dest=r%i\n", f_label, live, bif, arg1, dreg);

                    GCBifImpl1 func = (GCBifImpl1) mod->imported_funcs[bif].bif;
                    term ret = func(ctx, f_label, live, arg1);

                    WRITE_REGISTER(dreg_type, dreg, ret);
                #endif

                #ifdef IMPL_CODE_LOADER
                    TRACE("gc_bif1/5\n");

                    UNUSED(f_label)
                    UNUSED(live)
                    UNUSED(bif)
                    UNUSED(arg1)
                    UNUSED(dreg)
                #endif

                UNUSED(f_label)

                NEXT_INSTRUCTION(next_off);
                break;
            }

            case OP_GC_BIF2: {
                int next_off = 1;
                int f_label;
                DECODE_LABEL(f_label, code, i, next_off, next_off);
                int live;
                DECODE_INTEGER(live, code, i, next_off, next_off);
                int bif;
                DECODE_INTEGER(bif, code, i, next_off, next_off); //s?
                term arg1;
                DECODE_COMPACT_TERM(arg1, code, i, next_off, next_off);
                term arg2;
                DECODE_COMPACT_TERM(arg2, code, i, next_off, next_off);
                int dreg;
                uint8_t dreg_type;
                DECODE_DEST_REGISTER(dreg, dreg_type, code, i, next_off, next_off);

                #ifdef IMPL_EXECUTE_LOOP
                    TRACE("gc_bif2/6 fail_lbl=%i, live=%i, bif=%i, arg1=0x%lx, arg2=0x%lx, dest=r%i\n", f_label, live, bif, arg1, arg2, dreg);

                    GCBifImpl2 func = (GCBifImpl2) mod->imported_funcs[bif].bif;
                    term ret = func(ctx, f_label, live, arg1, arg2);

                    WRITE_REGISTER(dreg_type, dreg, ret);
                #endif

                #ifdef IMPL_CODE_LOADER
                    TRACE("gc_bif2/6\n");

                    UNUSED(f_label)
                    UNUSED(live)
                    UNUSED(bif)
                    UNUSED(arg1)
                    UNUSED(arg2)
                    UNUSED(dreg)
                #endif

                UNUSED(f_label)

                NEXT_INSTRUCTION(next_off);
                break;
            }

            case OP_TRIM: {
                int next_offset = 1;
                int n_words;
                DECODE_INTEGER(n_words, code, i, next_offset, next_offset);
                int n_remaining;
                DECODE_INTEGER(n_remaining, code, i, next_offset, next_offset);

                TRACE("trim/2 words=%i, remaining=%i\n", n_words, n_remaining);
                USED_BY_TRACE(n_words);
                USED_BY_TRACE(n_remaining);

                #ifdef IMPL_EXECUTE_LOOP
                    DEBUG_DUMP_STACK(ctx);
                    ctx->e += n_words;
                    DEBUG_DUMP_STACK(ctx);
                #endif

                UNUSED(n_remaining)

                NEXT_INSTRUCTION(next_offset);
                break;
            }

            case OP_LINE: {
                int next_offset = 1;
                int line_number;
                DECODE_INTEGER(line_number, code, i, next_offset, next_offset);

                TRACE("line/1: %i\n", line_number);

                NEXT_INSTRUCTION(next_offset);
                break;
            }

            default:
                printf("Undecoded opcode: %i\n", code[i]);
                #ifdef IMPL_EXECUTE_LOOP
                    fprintf(stderr, "failed at %i\n", i);
                #endif

                abort();
                return 1;
        }
    }
}

#pragma GCC diagnostic pop

#undef DECODE_COMPACT_TERM
