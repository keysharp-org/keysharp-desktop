#ifndef KEYSHARP_DESKTOP_OPERATION_SCOPE_H
#define KEYSHARP_DESKTOP_OPERATION_SCOPE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Classification of desktop opcodes. ksd_operation_bit names the backend
 * operation an opcode drives, ksd_operation_scope the permission scope the
 * grant check demands, and ksd_operation_scope_free the short list of opcodes
 * that are allowed to run with no scope at all. An opcode with an operation
 * bit, no scope and no scope-free entry is unclassified and is denied.
 * ksd_request_chunk_admissible is the whole frame-level rule the authority
 * applies before a sequence may start.
 * ksd_operation_chunkable names the opcodes whose request payload may arrive
 * as a KSD_FLAG_MORE sequence; every other opcode ends the session on the
 * first chunk. Only the clipboard write is chunkable, and it is scope free,
 * so a session with no grant can spend one assembly reservation; the per-user
 * and global assembly budgets bound that. tests/operation_scope_tests.c pins
 * all four over the whole opcode space.
 */
uint32_t ksd_operation_scope(uint16_t opcode);
bool ksd_operation_scope_free(uint16_t opcode);
bool ksd_operation_chunkable(uint16_t opcode);
bool ksd_request_chunk_admissible(uint16_t opcode, uint16_t flags,
                                 uint64_t request_id);
uint64_t ksd_operation_bit(uint16_t opcode);

#endif
