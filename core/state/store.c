//! An ordered map of byte keys to byte values on tidwall's copy-on-write B-tree, for store.bob, which alone imports
//! this file. An entry is one malloc'd, reference-counted record, which clones share. Keys order bytewise, a shorter
//! key first on a tie. A function that cannot allocate leaves the store as it was and returns 0 for a handle or -1 for
//! a result. The counts, the library's and the records', are atomic, so that clones may be read in other threads.

#include <stdatomic.h>
#include <stdint.h>  // which btree.c takes from its header, left out as the library is static here
#define BTREE_STATIC
#include "../../vendor/btree/btree.c"

// An entry: its key's bytes then its value's, held by each node that has it.
typedef struct {
  atomic_size_t refs;
  size_t klen, vlen;
  const unsigned char *key;  // data, or for a probe the caller's bytes
  unsigned char data[];
} rec;

typedef struct {
  struct btree *tree;  // of rec *
  size_t bytes;
} store;

static atomic_long records, trees;
static long failing;

/// Fails the nth allocation from now, for a test; none for 0.
void store_fail(long n) { failing = n; }

static void *alloc(size_t n) { return failing && !--failing ? 0 : malloc(n); }

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
  if (atomic_fetch_sub(&r->refs, 1) == 1) free(r), atomic_fetch_sub(&records, 1);
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

/// 0 once key holds value.
int store_put(void *p, const void *key, size_t kn, const void *value, size_t vn) {
  store *s = p;
  rec *old = find(s, key, kn), *r = alloc(sizeof *r + kn + vn);
  if (!r) return -1;
  r->refs = 1, r->klen = kn, r->vlen = vn, r->key = r->data, atomic_fetch_add(&records, 1);
  memcpy(r->data, key, kn), memcpy(r->data + kn, value, vn);
  size_t was = old ? kn + old->vlen : 0;  // read now, as the set frees old
  btree_set(s->tree, &r);  // which holds r, or on failure not
  release(r);
  if (btree_oom(s->tree)) return -1;
  s->bytes += kn + vn - was;
  return 0;
}

/// 1 with out the value's pointer and length, valid until s next changes; 0 if absent.
int store_get(void *s, const void *key, size_t kn, uint64_t *out) {
  rec *r = find(s, key, kn);
  if (r) out[0] = (uintptr_t)(r->data + kn), out[1] = r->vlen;
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
  if (r->klen < c->prefix->klen || memcmp(r->key, c->prefix->key, c->prefix->klen)) return false;
  if (c->after && !compare(item, &c->after, 0)) return true;
  uint64_t *o = c->out;
  o[0] = (uintptr_t)r->key, o[1] = r->klen, o[2] = (uintptr_t)(r->key + r->klen), o[3] = r->vlen;
  c->out += 4;
  return --c->left;
}

/// Up to limit entries whose keys start with prefix and, unless after is null, come after it, in order, as 4 words
/// each in out: the key's pointer and length, the value's pointer and length, valid until s next changes. Their number.
size_t store_scan(void *s, const void *prefix, size_t pn, const void *after, size_t an, size_t limit, uint64_t *out) {
  rec p = {.klen = pn, .key = prefix}, a = {.klen = an, .key = after};
  scan c = {&p, after ? &a : 0, limit, out};
  const rec *from = after && keycmp(after, an, prefix, pn) > 0 ? &a : &p;
  if (limit) btree_ascend(((store *)s)->tree, &from, visit, &c);
  return limit - c.left;
}

size_t store_count(void *s) { return btree_count(((store *)s)->tree); }
/// The bytes of the keys and values s holds.
size_t store_bytes(void *s) { return ((store *)s)->bytes; }
/// The records, and the stores, not yet freed in the process.
long store_records(void) { return atomic_load(&records); }
long store_trees(void) { return atomic_load(&trees); }
