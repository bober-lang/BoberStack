//! An ordered map of byte keys to byte values on tidwall's copy-on-write B-tree, for store.bob, which alone imports
//! this file. An entry is one malloc'd, reference-counted record, which clones share. Keys order bytewise, a shorter
//! key first on a tie. A function that cannot allocate leaves the store as it was and returns 0 for a handle or -1 for
//! a result. The counts, the library's and the records', and the list of the released are atomic, so that clones may
//! be read in other threads.
//!
//! A record may also hold a handle, which only store.bob interprets: of the blob it put the record with. One that does
//! is not freed with its last holder but put on the list of the released, which store_released empties: so store.bob
//! learns of it, and releasing a record still allocates nothing and cannot fail. The list is the process's and a handle
//! means something only to the isolate that put it, so records with handles are to be put and freed in one isolate.

#include <stdatomic.h>
#include <stdint.h>  // which btree.c takes from its header, left out as the library is static here
#define BTREE_STATIC
#include "../../vendor/btree/btree.c"

static atomic_long records, trees;
static long failing;

/// Fails the nth allocation from now, for a test; none for 0.
void store_fail(long n) { failing = n; }

static bool fail(void) { return failing && !--failing; }
static void *alloc(size_t n) { return fail() ? 0 : malloc(n); }

// An entry: its key's bytes then its value's, held by each node that has it.
typedef struct rec {
  atomic_size_t refs;
  size_t klen, vlen;
  const unsigned char *key;  // data, or for a probe the caller's bytes
  uint64_t handle;
  struct rec *next;  // on the released list
  unsigned char data[];
} rec;

static _Atomic(rec *) released;

// Pushes the chain from first to last onto the released list.
static void push(rec *first, rec *last) {
  last->next = atomic_load(&released);
  while (!atomic_compare_exchange_weak(&released, &last->next, first)) continue;
}

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
  if (r->handle) push(r, r);
  else free(r), atomic_fetch_sub(&records, 1);
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

/// 0 once key holds value and handle, 0 for none, which store_released gives back once the record is freed.
int store_put(void *p, const void *key, size_t kn, const void *value, size_t vn, uint64_t handle) {
  store *s = p;
  rec *old = find(s, key, kn), *r = alloc(sizeof *r + kn + vn);
  if (!r) return -1;
  r->refs = 1, r->klen = kn, r->vlen = vn, r->key = r->data, r->handle = handle, atomic_fetch_add(&records, 1);
  memcpy(r->data, key, kn), memcpy(r->data + kn, value, vn);
  size_t was = old ? kn + old->vlen : 0;  // read now, as the set frees old
  btree_set(s->tree, &r);  // which holds r, or on failure not
  bool failed = btree_oom(s->tree);
  if (failed) r->handle = 0;  // as a put that failed reports no release
  release(r);
  if (failed) return -1;
  s->bytes += kn + vn - was;
  return 0;
}

/// 1 with out the value's pointer and length, valid until s next changes, and its handle; 0 if absent.
int store_get(void *s, const void *key, size_t kn, uint64_t *out) {
  rec *r = find(s, key, kn);
  if (r) out[0] = (uintptr_t)(r->data + kn), out[1] = r->vlen, out[2] = r->handle;
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

/// Frees up to n of the released records, their handles into out; their number.
size_t store_released(uint64_t *out, size_t n) {
  if (!atomic_load(&released)) return 0;
  rec *r = atomic_exchange(&released, 0);
  size_t i = 0;
  for (; r && i < n; i++) {
    rec *next = r->next;
    out[i] = r->handle, free(r), atomic_fetch_sub(&records, 1);
    r = next;
  }
  if (r) {
    rec *last = r;
    while (last->next) last = last->next;
    push(r, last);
  }
  return i;
}

size_t store_count(void *s) { return btree_count(((store *)s)->tree); }
/// The bytes of the keys and values s holds.
size_t store_bytes(void *s) { return ((store *)s)->bytes; }
/// Into out the records, released ones too, and the stores not yet freed in the process.
void store_live(int64_t *out) { out[0] = atomic_load(&records), out[1] = atomic_load(&trees); }
