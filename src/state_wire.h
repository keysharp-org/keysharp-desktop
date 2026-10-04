#ifndef KEYSHARP_DESKTOP_STATE_WIRE_H
#define KEYSHARP_DESKTOP_STATE_WIRE_H

#include "protocol_io.h"
#include "keysharp_desktop/client.h"

typedef bool (*ksd_json_window_fn)(ksd_window_record *, void *);
bool ksd_state_window_json(const uint8_t *, size_t, ksd_window_record *);
bool ksd_state_window_is_child_json(const uint8_t *, size_t);
bool ksd_state_windows_json(const uint8_t *, size_t, ksd_json_window_fn, void *);
bool ksd_state_event_encode(const ksd_state_event *, ksd_buffer *);
bool ksd_state_event_decode(const uint8_t *, size_t, ksd_state_event *);
bool ksd_state_window_equal(const ksd_window_record *, const ksd_window_record *);
bool ksd_state_window_copy(ksd_window_record *, const ksd_window_record *);
bool ksd_state_mutates_windows(uint16_t opcode);

#endif
