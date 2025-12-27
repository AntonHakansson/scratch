typedef struct S8  { unsigned char *data; intptr_t len; } S8;
typedef struct Cut { S8 head; S8 tail; } Cut;

static Cut cut(S8 s, char sep) {
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

static uint64_t s8hash(S8 s) {
  uint64_t h = 1111111111111111111u;
  for (int i = 0; i < s.len; i++) {
    h ^= s.data[i];
    h *= h;
  }
  h ^= h >> 32;
  return h;
}

static int s8cmp(S8 s1, S8 s2) {
  long min_len = s1.len < s2.len ? s1.len : s2.len;
  return strncmp((const char *)s1.data, (const char *)s2.data, min_len);
}

static intptr_t hash_table_idx_lookup(uintptr_t hash, intptr_t idx, uintptr_t exp) {
  uintptr_t mask = (1ull << exp) - 1;
  uintptr_t step = (hash >> (sizeof(hash)*8ull - exp)) | 1;
  return (idx + step) & mask;
}
