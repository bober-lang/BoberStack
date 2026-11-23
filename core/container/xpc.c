//! XPC messages for apple.bob, which alone imports this file: a message is a dictionary of typed values, sent to a
//! Mach service, which answers with another; cx_release frees either. A send opens a session of its own and blocks its
//! thread until the answer comes, so that any isolate may send.

#ifdef __APPLE__

#include <xpc/xpc.h>

// Of <xpc/session.h> and <xpc/rich_error.h>, which <xpc/xpc.h> includes only for a compiler with blocks, as bober's is
// not. None of these takes a block, and a session, unlike a connection, needs no event handler.
void *xpc_session_create_mach_service(const char *name, void *queue, uint64_t flags, void **error);
xpc_object_t xpc_session_send_message_with_reply_sync(void *session, xpc_object_t message, void **error);
void xpc_session_cancel(void *session);
char *xpc_rich_error_copy_description(void *error);

void *cx_new(void) { return xpc_dictionary_create_empty(); }

void cx_release(void *o) { xpc_release(o); }

void cx_set_string(void *m, const char *key, const char *value) { xpc_dictionary_set_string(m, key, value); }

void cx_set_data(void *m, const char *key, const void *bytes, size_t n) { xpc_dictionary_set_data(m, key, bytes, n); }

void cx_set_bool(void *m, const char *key, bool value) { xpc_dictionary_set_bool(m, key, value); }

void cx_set_int(void *m, const char *key, int64_t value) { xpc_dictionary_set_int64(m, key, value); }

/// The answer to m from service, or 0 with *error a malloc'd description of the failure.
void *cx_send(const char *service, void *m, char **error) {
  void *e = 0;
  void *s = xpc_session_create_mach_service(service, 0, 0, &e);
  xpc_object_t r = s ? xpc_session_send_message_with_reply_sync(s, m, &e) : 0;
  if (s) xpc_session_cancel(s), xpc_release(s);
  if (!r) *error = e ? xpc_rich_error_copy_description(e) : strdup("no session");
  if (e) xpc_release(e);
  return r;
}

/// The bytes of the string or data at key, valid while m is, their length in *n; 0 if there is neither.
const void *cx_bytes(void *m, const char *key, size_t *n) {
  xpc_object_t v = xpc_dictionary_get_value(m, key);
  xpc_type_t t = v ? xpc_get_type(v) : 0;
  if (t == XPC_TYPE_STRING) return *n = xpc_string_get_length(v), xpc_string_get_string_ptr(v);
  if (t == XPC_TYPE_DATA) return *n = xpc_data_get_length(v), xpc_data_get_bytes_ptr(v);
  return 0;
}

int64_t cx_int(void *m, const char *key) { return xpc_dictionary_get_int64(m, key); }

/// The file of the first descriptor in the array at key, read whole, malloc'd, its length in *n; 0 if none or failed.
char *cx_file(void *m, const char *key, size_t *n) {
  xpc_object_t fds = xpc_dictionary_get_value(m, key);
  int fd = fds ? xpc_array_dup_fd(fds, 0) : -1;
  if (fd < 0) return 0;
  size_t cap = 1 << 16, len = 0;
  char *b = malloc(cap), *grown;
  ssize_t got = 0;
  while (b && (got = pread(fd, b + len, cap - len, len)) > 0)
    if ((len += got) == cap) grown = realloc(b, cap *= 2), grown ? (b = grown) : (free(b), b = 0);
  close(fd);
  if (got < 0) free(b), b = 0;
  *n = len;
  return b;
}

#else

// Only macOS has the daemon: elsewhere every send fails, and so nothing else here is reached.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

void *cx_new(void) { return 0; }
void cx_release(void *o) {}
void cx_set_string(void *m, const char *key, const char *value) {}
void cx_set_data(void *m, const char *key, const void *bytes, size_t n) {}
void cx_set_bool(void *m, const char *key, bool value) {}
void cx_set_int(void *m, const char *key, int64_t value) {}
void *cx_send(const char *service, void *m, char **error) { return *error = strdup("XPC is macOS's alone"), (void *)0; }
const void *cx_bytes(void *m, const char *key, size_t *n) { return 0; }
int64_t cx_int(void *m, const char *key) { return 0; }
char *cx_file(void *m, const char *key, size_t *n) { return 0; }

#endif
