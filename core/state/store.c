//! An ordered map of byte keys to byte values on tidwall's copy-on-write B-tree, for store.bob, which alone imports
//! this file. An entry is one malloc'd, reference-counted record, which clones share. Keys order bytewise, a shorter
//! key first on a tie. A function that cannot allocate leaves the store as it was and returns 0 for a handle or -1 for
//! a result. The counts, the library's and the records', are atomic, so that clones may be read in other threads.
//!
//! A record may also hold a blob: bytes too large to copy into it, as an S3 object's, in chunks of unlinked temporary
//! files under $TMPDIR, else /tmp, mapped into memory, where the kernel may page them out. A blob is a list of spans of
//! chunks, so that joining blobs and holding one under several keys copy nothing. Blobs and chunks are
//! reference-counted, held by records and by store.bob's handles, so that a chunk is unmapped, and its file's space
//! returned, once the last record or handle that reaches it is freed.

#include <stdatomic.h>
#include <stdint.h>  // which btree.c takes from its header, left out as the library is static here
#define BTREE_STATIC
#include "../../vendor/btree/btree.c"
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

// The bytes a chunk holds at most: a blob's writer starts another past them.
#define CHUNK (4 << 20)

static atomic_long records, trees, blobs, chunks;
static long failing;

/// Fails the nth allocation from now, for a test; none for 0. Making a chunk's file counts as one.
void store_fail(long n) { failing = n; }

static bool fail(void) { return failing && !--failing; }
static void *alloc(size_t n) { return fail() ? 0 : malloc(n); }

// A file's bytes, mapped read-only, and unmapped with its last reference.
typedef struct {
  atomic_size_t refs;
  size_t len;
  unsigned char *at;
} chunk;

// Bytes of a chunk, from the blob's start offset on.
typedef struct {
  chunk *c;
  size_t from, len, start;
} span;

typedef struct {
  atomic_size_t refs;
  size_t size, n;
  span spans[];
} blob;

// A blob being written: the spans sealed, and the file of the chunk being filled, -1 if none.
typedef struct {
  int fd;
  size_t filled, size, n, cap;
  span *spans;
} writer;

static void unchunk(chunk *c) {
  if (atomic_fetch_sub(&c->refs, 1) == 1) munmap(c->at, c->len), free(c), atomic_fetch_sub(&chunks, 1);
}

static void unspan(span *s, size_t n) {
  for (size_t i = 0; i < n; i++) unchunk(s[i].c);
}

/// Takes another reference to b if n is 1; gives one up if -1, which frees b with the last.
void blob_ref(void *p, int n) {
  blob *b = p;
  if (n > 0) atomic_fetch_add(&b->refs, 1);
  else if (atomic_fetch_sub(&b->refs, 1) == 1) unspan(b->spans, b->n), free(b), atomic_fetch_sub(&blobs, 1);
}

/// A blob's writer, empty.
void *blob_new(void) {
  writer *w = alloc(sizeof *w);
  if (w) *w = (writer){.fd = -1}, atomic_fetch_add(&blobs, 1);
  return w;
}

// A temporary file, unlinked, -1 if none.
static int tempfile(void) {
  if (fail()) return -1;
  const char *dir = getenv("TMPDIR");
  char path[4096];
  if (snprintf(path, sizeof path, "%s/boberstack-XXXXXX", dir && *dir ? dir : "/tmp") >= (int)sizeof path) return -1;
  int fd = mkstemp(path);
  if (fd >= 0) unlink(path), atomic_fetch_add(&chunks, 1);
  return fd;
}

static void unfile(writer *w) {
  if (w->fd >= 0) close(w->fd), w->fd = -1, w->filled = 0, atomic_fetch_sub(&chunks, 1);
}

// Maps the chunk being filled and adds it to w's spans; 0 if it has, or there is none.
static int seal(writer *w) {
  if (w->fd < 0) return 0;
  if (w->n == w->cap) {
    size_t cap = w->cap ? 2 * w->cap : 4;
    span *s = alloc(cap * sizeof *s);
    if (!s) return -1;
    if (w->n) memcpy(s, w->spans, w->n * sizeof *s);
    free(w->spans), w->spans = s, w->cap = cap;
  }
  chunk *c = alloc(sizeof *c);
  void *at = c ? mmap(0, w->filled, PROT_READ, MAP_SHARED, w->fd, 0) : MAP_FAILED;
  if (at == MAP_FAILED) return free(c), -1;
  *c = (chunk){1, w->filled, at};
  w->spans[w->n++] = (span){c, 0, w->filled, w->size - w->filled};
  close(w->fd), w->fd = -1, w->filled = 0;
  return 0;
}

/// 0 once w holds n more bytes, p's.
int blob_write(void *p, const unsigned char *bytes, size_t n) {
  writer *w = p;
  while (n) {
    if (w->filled == CHUNK && seal(w)) return -1;
    if (w->fd < 0 && (w->fd = tempfile()) < 0) return -1;
    size_t room = CHUNK - w->filled;
    ssize_t k = write(w->fd, bytes, n < room ? n : room);
    if (k < 0 && errno == EINTR) continue;
    if (k <= 0) return -1;  // and what this call wrote stays, as a writer that fails is only aborted
    w->filled += k, w->size += k, bytes += k, n -= k;
  }
  return 0;
}

/// Frees w and all it wrote.
void blob_abort(void *p) {
  writer *w = p;
  unfile(w), unspan(w->spans, w->n), free(w->spans), free(w), atomic_fetch_sub(&blobs, 1);
}

/// The blob of what w wrote, which it frees.
void *blob_seal(void *p) {
  writer *w = p;
  blob *b = seal(w) ? 0 : alloc(sizeof *b + w->n * sizeof(span));
  if (!b) return 0;
  b->refs = 1, b->size = w->size, b->n = w->n;
  if (w->n) memcpy(b->spans, w->spans, w->n * sizeof(span));
  free(w->spans), free(w);
  return b;
}

/// A blob of the n blobs at ps in order, sharing their chunks.
void *blob_join(const uint64_t *ps, size_t n) {
  size_t spans = 0;
  for (size_t i = 0; i < n; i++) spans += ((blob *)(uintptr_t)ps[i])->n;
  blob *b = alloc(sizeof *b + spans * sizeof(span));
  if (!b) return 0;
  b->refs = 1, b->size = 0, b->n = 0, atomic_fetch_add(&blobs, 1);
  for (size_t i = 0; i < n; i++) {
    const blob *p = (blob *)(uintptr_t)ps[i];
    for (size_t j = 0; j < p->n; j++) {
      span s = p->spans[j];
      atomic_fetch_add(&s.c->refs, 1);
      s.start = b->size, b->size += s.len, b->spans[b->n++] = s;
    }
  }
  return b;
}

size_t blob_size(void *b) { return ((blob *)b)->size; }

/// Into out the pointer and length of the bytes of b from at on, up to n, that one chunk holds; valid while b is held.
void blob_span(void *p, size_t at, size_t n, uint64_t *out) {
  const blob *b = p;
  size_t lo = 0, hi = b->n;
  while (hi - lo > 1) {
    size_t mid = (lo + hi) / 2;
    if (b->spans[mid].start <= at) lo = mid;
    else hi = mid;
  }
  const span *s = &b->spans[lo];
  size_t in = at - s->start, left = at < b->size ? s->len - in : 0;
  out[0] = left ? (uintptr_t)(s->c->at + s->from + in) : 0, out[1] = n < left ? n : left;
}

// An entry: its key's bytes then its value's, and the blob it holds, if any; held by each node that has it.
typedef struct {
  atomic_size_t refs;
  size_t klen, vlen;
  const unsigned char *key;  // data, or for a probe the caller's bytes
  blob *blob;
  unsigned char data[];
} rec;

typedef struct {
  struct btree *tree;  // of rec *
  size_t bytes;
} store;

static int keycmp(const unsigned char *a, size_t an, const unsigned char *b, size_t bn) {
  int c = memcmp(a, b, an < bn ? an : bn);
  return c ? c : (an > bn) - (an < bn);
}

static int compare(const void *a, const void *b, void *udata) {
  const rec *x = *(rec *const *)a, *y = *(rec *const *)b;
  return keycmp(x->key, x->klen, y->key, y->klen);
}

static bool hold(const void *item, void *into, void *udata) {
  rec *r = *(rec *const *)item;
  atomic_fetch_add(&r->refs, 1);
  *(rec **)into = r;
  return true;
}

static void release(rec *r) {
  if (atomic_fetch_sub(&r->refs, 1) > 1) return;
  if (r->blob) blob_ref(r->blob, -1);
  free(r), atomic_fetch_sub(&records, 1);
}

static void drop(const void *item, void *udata) { release(*(rec *const *)item); }

static void *wrap(struct btree *t, size_t bytes) {
  store *s = t ? alloc(sizeof *s) : 0;
  if (s) *s = (store){t, bytes}, atomic_fetch_add(&trees, 1);
  else if (t) btree_free(t);
  return s;
}

/// An empty store.
void *store_new(void) {
  struct btree *t = btree_new_with_allocator(alloc, 0, free, sizeof(rec *), 0, compare, 0);
  if (t) btree_set_item_callbacks(t, hold, drop);
  return wrap(t, 0);
}

/// A copy of s in O(1): the two share nodes and records until either writes.
void *store_clone(void *s) { return wrap(btree_clone(((store *)s)->tree), ((store *)s)->bytes); }

void store_free(void *s) {
  btree_free(((store *)s)->tree);
  free(s), atomic_fetch_sub(&trees, 1);
}

static rec *find(store *s, const void *key, size_t kn) {
  rec probe = {.klen = kn, .key = key}, *p = &probe;
  rec *const *at = btree_get(s->tree, &p);
  return at ? *at : 0;
}

/// 0 once key holds value, and the blob b unless it is null, which the record then holds a reference to.
int store_put(void *p, const void *key, size_t kn, const void *value, size_t vn, void *b) {
  store *s = p;
  rec *old = find(s, key, kn), *r = alloc(sizeof *r + kn + vn);
  if (!r) return -1;
  r->refs = 1, r->klen = kn, r->vlen = vn, r->key = r->data, r->blob = b, atomic_fetch_add(&records, 1);
  if (b) blob_ref(b, 1);
  memcpy(r->data, key, kn), memcpy(r->data + kn, value, vn);
  size_t was = old ? kn + old->vlen : 0;  // read now, as the set frees old
  btree_set(s->tree, &r);  // which holds r, or on failure not
  release(r);
  if (btree_oom(s->tree)) return -1;
  s->bytes += kn + vn - was;
  return 0;
}

/// 1 with out the value's pointer and length, valid until s next changes, and its blob, 0 if none; 0 if absent.
int store_get(void *s, const void *key, size_t kn, uint64_t *out) {
  rec *r = find(s, key, kn);
  if (r) out[0] = (uintptr_t)(r->data + kn), out[1] = r->vlen, out[2] = (uintptr_t)r->blob;
  return r != 0;
}

/// 1 if key was there, 0 if not.
int store_delete(void *p, const void *key, size_t kn) {
  store *s = p;
  rec *r = find(s, key, kn);
  if (!r) return 0;
  size_t n = kn + r->vlen;
  btree_delete(s->tree, &r);
  if (btree_oom(s->tree)) return -1;
  s->bytes -= n;
  return 1;
}

typedef struct {
  const rec *prefix, *after;
  size_t left;
  uint64_t *out;
} scan;

static bool visit(const void *item, void *udata) {
  const rec *r = *(rec *const *)item;
  scan *c = udata;
  if (c->after && !compare(item, &c->after, 0)) return true;
  if (r->klen < c->prefix->klen || memcmp(r->key, c->prefix->key, c->prefix->klen)) return false;
  uint64_t *o = c->out;
  o[0] = (uintptr_t)r->key, o[1] = r->klen, o[2] = (uintptr_t)(r->key + r->klen), o[3] = r->vlen;
  c->out += 4;
  return --c->left;
}

/// Up to limit entries whose keys start with prefix and, unless after is null, come after it, in order, as 4 words
/// each in out: the key's pointer and length, the value's pointer and length, valid until s next changes. Their number.
/// Descending, those before after, or from the last key if it is null: after must then not be above the least key
/// above the prefix's, nor null if there is one, as the scan stops at the first key past the prefix's.
size_t store_scan(void *s, const void *prefix, size_t pn, const void *after, size_t an, int descending, size_t limit,
                  uint64_t *out) {
  rec p = {.klen = pn, .key = prefix}, a = {.klen = an, .key = after};
  scan c = {&p, after ? &a : 0, limit, out};
  const rec *from = after && (descending || keycmp(after, an, prefix, pn) > 0) ? &a : &p;
  if (limit && descending) btree_descend(((store *)s)->tree, after ? &from : 0, visit, &c);
  else if (limit) btree_ascend(((store *)s)->tree, &from, visit, &c);
  return limit - c.left;
}

size_t store_count(void *s) { return btree_count(((store *)s)->tree); }
/// The bytes of the keys and values s holds, its blobs' not counted.
size_t store_bytes(void *s) { return ((store *)s)->bytes; }
/// Into out the records, the stores, the blobs and writers, and the chunks, not yet freed in the process.
void store_live(int64_t *out) {
  out[0] = atomic_load(&records), out[1] = atomic_load(&trees), out[2] = atomic_load(&blobs);
  out[3] = atomic_load(&chunks);
}
