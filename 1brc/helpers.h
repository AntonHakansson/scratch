#define min(a, b) (((a) < (b)) ? (a) : (b))

typedef struct S8  { unsigned char *data; intptr_t len; } S8;
typedef struct Cut { S8 head; S8 tail; } Cut;

static Cut __attribute__((unused)) cut(S8 s, char sep) {
  PROF_FUNCTION_BEGIN;
  Cut r = {0};
  if (s.len) {
    r.head.data = s.data;
    while (s.len > 0 && *s.data != sep) {
      r.head.len++;
      s.data++; s.len--;
    }
    if (s.len) {
      s.data++; s.len--; // skip seperator
      r.tail = s;
    }
  }
  PROF_FUNCTION_END;
  return r;
}

static uint64_t hash_fnv1a(unsigned char *buf, uintptr_t len) {
  uint64_t hash = 0xcbf29ce484222325;
  while (len--) {
    hash ^= *(unsigned char*)buf;
    hash *= 0x00000100000001b3;
    buf++;
  }
  hash ^= hash >> 32;
  return hash;
}

static uint64_t s8hash(S8 s) {
  PROF_FUNCTION_BEGIN;
  uint64_t h = hash_fnv1a(s.data, s.len);
  PROF_FUNCTION_END;
  return h;
}

static int s8cmp(S8 s1, S8 s2) {
  long min_len = s1.len < s2.len ? s1.len : s2.len;
  int cmp = strncmp((const char *)s1.data, (const char *)s2.data, min_len);
  if (cmp != 0) return cmp;
  if (s1.len < s2.len) return -1;
  if (s1.len > s2.len) return 1;
  return 0;
}

static _Bool s8eq(S8 s1, S8 s2) {
  return (s1.len == s2.len) && memcmp(s1.data, s2.data, s1.len) == 0;
}

static intptr_t hash_table_idx_lookup(uintptr_t hash, intptr_t idx, uintptr_t exp) {
  uintptr_t mask = (1u << exp) - 1;
  uintptr_t step = (hash >> (sizeof(hash)*8 - exp)) | 1;
  return (idx + step) & mask;
}
