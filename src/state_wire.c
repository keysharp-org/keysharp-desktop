#include "state_wire.h"
#include "protocol.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct json_span { const uint8_t *data; size_t length; } json_span;

static void skip_space(json_span *s)
{
    while (s->length != 0u && (s->data[0] == ' ' || s->data[0] == '\n'
        || s->data[0] == '\r' || s->data[0] == '\t')) { s->data++; s->length--; }
}

static bool value_span(json_span *s, json_span *value, unsigned depth)
{
    if (depth > 64u) return false;
    skip_space(s);
    if (s->length == 0u) return false;
    const uint8_t *start = s->data;
    if (*s->data == '"') {
        s->data++; s->length--;
        while (s->length != 0u && *s->data != '"') {
            if (*s->data < 0x20u) return false;
            if (*s->data == '\\') {
                s->data++; s->length--;
                if (s->length == 0u) return false;
            }
            s->data++; s->length--;
        }
        if (s->length == 0u) return false;
        s->data++; s->length--;
    } else if (*s->data == '{' || *s->data == '[') {
        uint8_t end = *s->data == '{' ? '}' : ']';
        bool object = *s->data == '{';
        s->data++; s->length--; skip_space(s);
        while (s->length != 0u && *s->data != end) {
            json_span part;
            if (!value_span(s, &part, depth + 1u)) return false;
            if (object) {
                skip_space(s);
                if (part.data[0] != '"' || s->length == 0u || *s->data != ':') return false;
                s->data++; s->length--;
                if (!value_span(s, &part, depth + 1u)) return false;
            }
            skip_space(s);
            if (s->length == 0u) return false;
            if (*s->data == end) break;
            if (*s->data != ',') return false;
            s->data++; s->length--; skip_space(s);
            if (s->length == 0u || *s->data == end) return false;
        }
        if (s->length == 0u) return false;
        s->data++; s->length--;
    } else {
        while (s->length != 0u && *s->data != ',' && *s->data != '}'
            && *s->data != ']' && *s->data != ' ' && *s->data != '\n'
            && *s->data != '\r' && *s->data != '\t') { s->data++; s->length--; }
        if (s->data == start) return false;
    }
    *value = (json_span) { start, (size_t)(s->data - start) };
    return true;
}

static bool field(json_span object, const char *name, json_span *result)
{
    skip_space(&object);
    if (object.length < 2u || *object.data != '{') return false;
    object.data++; object.length--;
    size_t length = strlen(name);
    for (;;) {
        json_span key, value;
        if (!value_span(&object, &key, 0u)) return false;
        skip_space(&object);
        if (object.length == 0u || *object.data != ':') return false;
        object.data++; object.length--;
        if (!value_span(&object, &value, 0u)) return false;
        if (key.length == length + 2u && key.data[0] == '"'
            && memcmp(key.data + 1u, name, length) == 0) { *result = value; return true; }
        skip_space(&object);
        if (object.length == 0u || *object.data != ',') return false;
        object.data++; object.length--;
    }
}

static bool number(json_span s, uint64_t *value)
{
    if (s.length >= 2u && s.data[0] == '"') { s.data++; s.length -= 2u; }
    if (s.length == 0u || s.length > 20u) return false;
    uint64_t result = 0u;
    for (size_t i = 0u; i < s.length; i++) {
        if (s.data[i] < '0' || s.data[i] > '9') return false;
        unsigned digit = s.data[i] - '0';
        if (result > (UINT64_MAX - digit) / 10u) return false;
        result = result * 10u + digit;
    }
    *value = result;
    return true;
}

static uint64_t integer(json_span object, const char *name)
{
    json_span value; uint64_t result = 0u;
    if (field(object, name, &value)) (void)number(value, &result);
    return result;
}

static int32_t coordinate(json_span object, const char *name)
{
    json_span value; uint64_t result;
    if (!field(object, name, &value)) return 0;
    bool negative = value.length != 0u && value.data[0] == '-';
    if (negative) { value.data++; value.length--; }
    if (!number(value, &result) || result > (negative ? UINT64_C(2147483648) : INT32_MAX)) return 0;
    return negative ? (int32_t)(-(int64_t)result) : (int32_t)result;
}

static bool truth(json_span object, const char *name)
{
    json_span value;
    return field(object, name, &value) && value.length == 4u && memcmp(value.data, "true", 4u) == 0;
}

static int hex(uint8_t b)
{
    return b >= '0' && b <= '9' ? b - '0' : b >= 'a' && b <= 'f' ? b - 'a' + 10
        : b >= 'A' && b <= 'F' ? b - 'A' + 10 : -1;
}

static bool unicode(json_span *value, uint32_t *code)
{
    if (value->length < 4u) return false;
    *code = 0u;
    for (size_t i = 0u; i < 4u; i++) { int digit = hex(value->data[i]); if (digit < 0) return false; *code = *code * 16u + (unsigned)digit; }
    value->data += 4u; value->length -= 4u;
    return true;
}

static bool text(json_span value, ksd_string *out)
{
    if (value.length < 2u || value.data[0] != '"' || value.data[value.length - 1u] != '"') return false;
    value.data++; value.length -= 2u;
    ksd_buffer bytes; ksd_buffer_init(&bytes, KSD_MAX_TEXT_BYTES);
    while (value.length != 0u) {
        uint8_t b = *value.data++; value.length--;
        if (b == '\\') {
            if (value.length == 0u) goto fail;
            b = *value.data++; value.length--;
            if (b == 'u') {
                uint32_t code;
                if (!unicode(&value, &code)) goto fail;
                if (code >= 0xd800u && code <= 0xdbffu) {
                    if (value.length < 6u || value.data[0] != '\\' || value.data[1] != 'u') goto fail;
                    value.data += 2u; value.length -= 2u;
                    uint32_t low; if (!unicode(&value, &low) || low < 0xdc00u || low > 0xdfffu) goto fail;
                    code = 0x10000u + ((code - 0xd800u) << 10u) + low - 0xdc00u;
                } else if (code >= 0xdc00u && code <= 0xdfffu) goto fail;
                uint8_t encoded[4]; size_t n;
                if (code < 0x80u) { encoded[0] = (uint8_t)code; n = 1u; }
                else if (code < 0x800u) { encoded[0] = (uint8_t)(0xc0u | code >> 6u); encoded[1] = (uint8_t)(0x80u | (code & 63u)); n = 2u; }
                else if (code < 0x10000u) { encoded[0] = (uint8_t)(0xe0u | code >> 12u); encoded[1] = (uint8_t)(0x80u | (code >> 6u & 63u)); encoded[2] = (uint8_t)(0x80u | (code & 63u)); n = 3u; }
                else { encoded[0] = (uint8_t)(0xf0u | code >> 18u); encoded[1] = (uint8_t)(0x80u | (code >> 12u & 63u)); encoded[2] = (uint8_t)(0x80u | (code >> 6u & 63u)); encoded[3] = (uint8_t)(0x80u | (code & 63u)); n = 4u; }
                if (!ksd_buffer_bytes(&bytes, encoded, n)) goto fail;
                continue;
            }
            switch (b) {
                case 'n': b = '\n'; break; case 'r': b = '\r'; break; case 't': b = '\t'; break;
                case 'b': b = '\b'; break; case 'f': b = '\f'; break;
                case '"': case '\\': case '/': break; default: goto fail;
            }
        }
        if (!ksd_buffer_bytes(&bytes, &b, 1u)) goto fail;
    }
    if (!ksd_utf8_valid(bytes.data, bytes.length, false) || !ksd_buffer_bytes(&bytes, "", 1u)) goto fail;
    out->data = (char *)bytes.data; out->length = bytes.length - 1u;
    return true;
fail:
    ksd_buffer_clear(&bytes); return false;
}

void ksd_window_record_init(ksd_window_record *window)
{
    if (window == NULL) return;
    memset(window, 0, sizeof(*window)); window->struct_size = sizeof(*window);
    window->title.struct_size = window->app_id.struct_size = window->capture_id.struct_size = window->compositor_id.struct_size = sizeof(ksd_string);
}

void ksd_window_record_clear(ksd_window_record *window)
{
    if (window == NULL || window->struct_size < sizeof(*window)) return;
    free(window->title.data); free(window->app_id.data); free(window->capture_id.data); free(window->compositor_id.data);
    ksd_window_record_init(window);
}

void ksd_state_event_init(ksd_state_event *event)
{
    if (event == NULL) return;
    memset(event, 0, sizeof(*event)); event->struct_size = sizeof(*event);
    ksd_window_record_init(&event->window); event->data.struct_size = sizeof(ksd_string);
}

void ksd_state_event_clear(ksd_state_event *event)
{
    if (event == NULL || event->struct_size < sizeof(*event)) return;
    ksd_window_record_clear(&event->window); free(event->data.data); ksd_state_event_init(event);
}

bool ksd_state_window_json(const uint8_t *data, size_t length, ksd_window_record *window)
{
    json_span object = { data, length }, value;
    ksd_window_record_init(window);
    if (!field(object, "id", &value) && field(object, "window", &value)) object = value;
    if (!field(object, "id", &value) || !number(value, &window->handle) || window->handle == 0u) return false;
    window->valid_fields = KSD_FIELD_ID;
    const char *names[] = { "id", "title", "appId", "pid", "frame", "client", "visible", "active", "minimized", "maximized", "alwaysOnTop", "decorated", "transparency", "onCurrentWorkspace", "captureId", "parent", "surface", "buffer", "compositorId" };
    json_span fields;
    bool has_fields = field(object, "validFields", &fields);
    for (size_t i = 1u; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!field(object, names[i], &value)) continue;
        bool valid = !has_fields;
        if (has_fields && fields.length >= 2u) {
            json_span list = fields; list.data++; list.length--;
            while (value_span(&list, &value, 0u)) {
                size_t n = strlen(names[i]);
                if (value.length == n + 2u && memcmp(value.data + 1u, names[i], n) == 0) valid = true;
                skip_space(&list); if (list.length == 0u || list.data[0] != ',') break;
                list.data++; list.length--;
            }
        }
        if (valid) window->valid_fields |= UINT64_C(1) << i;
    }
    ksd_string *strings[] = { &window->title, &window->app_id, &window->capture_id, &window->compositor_id };
    const char *string_names[] = { "title", "appId", "captureId", "compositorId" };
    for (unsigned i = 0u; i < 4u; i++)
        if (field(object, string_names[i], &value) && !text(value, strings[i])) goto fail;
    window->pid = (uint32_t)integer(object, "pid"); window->parent = integer(object, "parent");
    window->transparency = (uint32_t)integer(object, "transparency");
    const char *rect_names[] = { "frame", "client", "surface" };
    for (unsigned i = 0u; i < 3u; i++) {
        if (!field(object, rect_names[i], &value)) continue;
        int32_t *rect = i == 0u ? &window->frame_x : i == 1u ? &window->client_x : &window->surface_x;
        rect[0] = coordinate(value, "x"); rect[1] = coordinate(value, "y");
        uint32_t *size = (uint32_t *)(rect + 2u); size[0] = (uint32_t)integer(value, "width"); size[1] = (uint32_t)integer(value, "height");
    }
    if (field(object, "buffer", &value)) { window->buffer_width = (uint32_t)integer(value, "width"); window->buffer_height = (uint32_t)integer(value, "height"); }
    if ((window->valid_fields & KSD_FIELD_BUFFER) != 0u
        && !field(object, "surface", &value) && field(object, "buffer", &value)) {
        window->surface_x = coordinate(value, "x"); window->surface_y = coordinate(value, "y");
        window->surface_width = (uint32_t)integer(value, "width"); window->surface_height = (uint32_t)integer(value, "height");
        window->valid_fields |= KSD_FIELD_SURFACE;
    }
    const char *flags[] = { "visible", "active", "minimized", "maximized", "alwaysOnTop", "decorated", "onCurrentWorkspace", "topLevel" };
    for (unsigned i = 0u; i < 8u; i++) if (truth(object, flags[i])) window->flags |= 1u << i;
    return true;
fail:
    ksd_window_record_clear(window); return false;
}

bool ksd_state_window_is_child_json(const uint8_t *data, size_t length)
{
    json_span object = { data, length }, value;
    if (!field(object, "id", &value) && field(object, "window", &value)) object = value;
    uint64_t top = integer(object, "topLevel"), id = integer(object, "id");
    return id != 0u && top != 0u && id != top;
}

bool ksd_state_windows_json(const uint8_t *data, size_t length, ksd_json_window_fn callback, void *context)
{
    json_span object = { data, length }, windows;
    if (!field(object, "windows", &windows) || windows.length < 2u || windows.data[0] != '[') return false;
    windows.data++; windows.length--;
    uint64_t order = 0u;
    skip_space(&windows);
    while (windows.length != 0u && windows.data[0] != ']') {
        json_span value; ksd_window_record window;
        if (!value_span(&windows, &value, 0u) || !ksd_state_window_json(value.data, value.length, &window)) return false;
        window.stacking_order = order++; window.valid_fields |= KSD_FIELD_STACKING_ORDER;
        bool ok = callback(&window, context); ksd_window_record_clear(&window); if (!ok) return false;
        skip_space(&windows); if (windows.length == 0u) return false;
        if (windows.data[0] == ']') break;
        if (windows.data[0] != ',') return false;
        windows.data++; windows.length--;
    }
    return windows.length != 0u && windows.data[0] == ']';
}

static bool encode_text(ksd_buffer *out, const ksd_string *s)
{
    return s->length <= KSD_MAX_TEXT_BYTES && ksd_buffer_u32(out, (uint32_t)s->length) && ksd_buffer_bytes(out, s->data, s->length);
}

static bool decode_text(ksd_cursor *in, ksd_string *s)
{
    uint32_t length; const uint8_t *data;
    if (!ksd_cursor_u32(in, &length) || length > KSD_MAX_TEXT_BYTES || !ksd_cursor_bytes(in, length, &data) || !ksd_utf8_valid(data, length, false)) return false;
    if (length == 0u) return true;
    s->data = malloc((size_t)length + 1u); if (s->data == NULL) return false;
    memcpy(s->data, data, length); s->data[length] = '\0'; s->length = length; return true;
}

bool ksd_state_event_encode(const ksd_state_event *event, ksd_buffer *out)
{
    const ksd_window_record *w = &event->window;
    bool ok = ksd_buffer_u32(out, event->kind) && ksd_buffer_u32(out, event->domain)
        && ksd_buffer_u64(out, event->epoch) && ksd_buffer_u64(out, event->sequence)
        && ksd_buffer_u32(out, event->granted_scopes) && ksd_buffer_u32(out, event->revoked_scopes)
        && ksd_buffer_u32(out, w->flags) && ksd_buffer_u64(out, w->valid_fields)
        && ksd_buffer_u64(out, w->handle) && ksd_buffer_u64(out, w->parent)
        && ksd_buffer_u32(out, w->pid) && ksd_buffer_u32(out, w->transparency);
    const uint32_t *values = (const uint32_t *)&w->frame_x;
    for (unsigned i = 0u; ok && i < 14u; i++) ok = ksd_buffer_u32(out, values[i]);
    return ok && ksd_buffer_u64(out, w->stacking_order) && encode_text(out, &w->title) && encode_text(out, &w->app_id)
        && encode_text(out, &w->capture_id) && encode_text(out, &w->compositor_id) && encode_text(out, &event->data);
}

bool ksd_state_event_decode(const uint8_t *data, size_t length, ksd_state_event *event)
{
    ksd_cursor in; ksd_cursor_init(&in, data, length); ksd_state_event_init(event);
    ksd_window_record *w = &event->window;
    bool ok = ksd_cursor_u32(&in, &event->kind) && ksd_cursor_u32(&in, &event->domain)
        && ksd_cursor_u64(&in, &event->epoch) && ksd_cursor_u64(&in, &event->sequence)
        && ksd_cursor_u32(&in, &event->granted_scopes) && ksd_cursor_u32(&in, &event->revoked_scopes)
        && ksd_cursor_u32(&in, &w->flags) && ksd_cursor_u64(&in, &w->valid_fields)
        && ksd_cursor_u64(&in, &w->handle) && ksd_cursor_u64(&in, &w->parent)
        && ksd_cursor_u32(&in, &w->pid) && ksd_cursor_u32(&in, &w->transparency);
    uint32_t *values = (uint32_t *)&w->frame_x;
    for (unsigned i = 0u; ok && i < 14u; i++) ok = ksd_cursor_u32(&in, &values[i]);
    ok = ok && ksd_cursor_u64(&in, &w->stacking_order) && decode_text(&in, &w->title) && decode_text(&in, &w->app_id)
        && decode_text(&in, &w->capture_id) && decode_text(&in, &w->compositor_id) && decode_text(&in, &event->data)
        && ksd_cursor_finished(&in) && event->kind >= KSD_STATE_SNAPSHOT_BEGIN && event->kind <= KSD_STATE_RESET
        && event->domain != 0u && (event->domain & (event->domain - 1u)) == 0u && (event->domain & ~KSD_STATE_ALL) == 0u;
    if (!ok) ksd_state_event_clear(event);
    return ok;
}

bool ksd_state_window_equal(const ksd_window_record *a, const ksd_window_record *b)
{
    if (memcmp(&a->flags, &b->flags, offsetof(ksd_window_record, title) - offsetof(ksd_window_record, flags)) != 0) return false;
    if (a->stacking_order != b->stacking_order) return false;
    const ksd_string *left[] = { &a->title, &a->app_id, &a->capture_id, &a->compositor_id };
    const ksd_string *right[] = { &b->title, &b->app_id, &b->capture_id, &b->compositor_id };
    for (unsigned i = 0u; i < 4u; i++) if (left[i]->length != right[i]->length || (left[i]->length != 0u && memcmp(left[i]->data, right[i]->data, left[i]->length) != 0)) return false;
    return true;
}

bool ksd_state_window_copy(ksd_window_record *out, const ksd_window_record *in)
{
    ksd_state_event event; ksd_state_event_init(&event); event.kind = KSD_STATE_CHANGED; event.domain = KSD_STATE_WINDOWS; event.window = *in;
    ksd_buffer bytes; ksd_buffer_init(&bytes, KSD_MAX_TEXT_BYTES); bool ok = ksd_state_event_encode(&event, &bytes);
    ksd_state_event copy; if (ok) ok = ksd_state_event_decode(bytes.data, bytes.length, &copy);
    ksd_buffer_clear(&bytes); if (!ok) return false;
    *out = copy.window; return true;
}

bool ksd_state_mutates_windows(uint16_t opcode)
{
    return (opcode >= KSD_OP_WINDOW_FOCUS && opcode <= KSD_OP_WINDOW_SET_SKIP_TASKBAR && opcode != KSD_OP_WINDOW_GET_RESERVED && opcode != KSD_OP_WINDOW_RESERVE)
        || opcode == KSD_OP_WINDOW_SET_TITLE || opcode == KSD_OP_WINDOW_SET_VISIBLE || opcode == KSD_OP_WINDOW_FOCUS_CHILD;
}

#if UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(ksd_window_record) == 272u, "window ABI layout");
_Static_assert(sizeof(ksd_state_event) == 360u, "state ABI layout");
#else
_Static_assert(sizeof(ksd_window_record) == 240u, "window ABI layout");
_Static_assert(sizeof(ksd_state_event) == 320u, "state ABI layout");
#endif
