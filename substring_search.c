#if IN_SHELL /* $ bash substring_search.c
 # cc substring_search.c -o substring_search -fsanitize=undefined -g3 -Wall -Wextra -Wconversion -Wno-sign-conversion -march=native $@
  cc substring_search.c -o substring_search -Os -march=native $@
exit # */
#endif

#define tassert(c)         while (!(c)) __builtin_trap()
#define countof(a)         (Iz)(sizeof(a) / sizeof(*(a)))
#define memset(d, c, sz)   __builtin_memset(d, c, sz)
#define memcmp(s1, s2, sz) __builtin_memcmp(s1, s2, sz)

#include <stdint.h>
#include <stddef.h>
typedef unsigned char U8;
typedef uint64_t      U64;
typedef int64_t       I64;
typedef intptr_t      Iz;
typedef uintptr_t     Uz;


////////////////////////////////////////////////////////////////////////////////
//- Rabin Karp

ptrdiff_t search_rabin_karp(const char *haystack, size_t haystack_len,
                                  const char *needle,   size_t needle_len) {
  if (needle_len == 0) { return 0; }
  if (haystack_len == 0) { return -1; }
  if (needle_len > haystack_len) { return -1; }

  enum { mult = 257, };

  uint64_t f = 1; {
    uint64_t x = mult;
    for (uintptr_t n = needle_len - 1; n; n >>= 1) {
      f *= n & 1 ? x : 1;
      x *= x;
    }
  }

  uint64_t match = 0;
  for (size_t i = 0; i < needle_len; i++) {
    match = match * mult + (unsigned char)needle[i];
  }

  const char *p = haystack;
  uint64_t rhash = 0;
  for (size_t i = 0; i < needle_len - 1; i++) {
    rhash = rhash * mult + (unsigned char)*p++;
  }

  for (; p < haystack + haystack_len; p++) {
    rhash = rhash * mult + (unsigned char)*p;
    if (rhash == match && memcmp(p - needle_len + 1, needle, needle_len) == 0) {
      return p - needle_len - haystack + 1;
    }
    rhash -= f * (unsigned char)p[1 - needle_len];
  }

  return -1;
}

#include <immintrin.h>
static ptrdiff_t search_rabin_karp_avx2(const char *haystack, size_t haystack_len,
                                        const char *needle,   size_t needle_len) {
  if (needle_len == 0) { return 0; }
  if (haystack_len == 0) { return -1; }
  if (needle_len > haystack_len) { return -1; }

  __m256i first = _mm256_set1_epi8(needle[0]);
  __m256i last = _mm256_set1_epi8(needle[needle_len - 1]);

  size_t i = 0;
  for (; needle_len <= 32 && i + 32 < haystack_len; i += 32) {
    __m256i in_first = _mm256_loadu_si256((__m256i *)(haystack + i));
    if (i + needle_len - 1 >= haystack_len) break;
    __m256i in_last = _mm256_loadu_si256((__m256i *)(haystack + i + needle_len - 1));

    __m256i hits_first = _mm256_cmpeq_epi8(first, in_first);
    __m256i hits_last = _mm256_cmpeq_epi8(last, in_last);

    uint32_t mask = _mm256_movemask_epi8(_mm256_and_si256(hits_first, hits_last));
    while (mask) {
      uint32_t bit = __builtin_ctz(mask);
      if (memcmp(haystack + i + bit + 1, needle + 1, needle_len - 1) == 0) {
        return i + bit;
      }
      mask &= mask - 1;
    }
  }

  ptrdiff_t fallback = search_rabin_karp(haystack + i, haystack_len - i,
                                         needle, needle_len);
  return fallback < 0 ? -1 : (i + fallback);
}

////////////////////////////////////////////////////////////////////////////////
//- musl strstr

#include <stddef.h>
#include <string.h>

static char *twobyte_strstr(const unsigned char *h, const unsigned char *n)
{
	uint16_t nw = n[0]<<8 | n[1], hw = h[0]<<8 | h[1];
	for (h++; *h && hw != nw; hw = hw<<8 | *++h);
	return *h ? (char *)h-1 : 0;
}

static char *threebyte_strstr(const unsigned char *h, const unsigned char *n)
{
	uint32_t nw = (uint32_t)n[0]<<24 | n[1]<<16 | n[2]<<8;
	uint32_t hw = (uint32_t)h[0]<<24 | h[1]<<16 | h[2]<<8;
	for (h+=2; *h && hw != nw; hw = (hw|*++h)<<8);
	return *h ? (char *)h-2 : 0;
}

static char *fourbyte_strstr(const unsigned char *h, const unsigned char *n)
{
	uint32_t nw = (uint32_t)n[0]<<24 | n[1]<<16 | n[2]<<8 | n[3];
	uint32_t hw = (uint32_t)h[0]<<24 | h[1]<<16 | h[2]<<8 | h[3];
	for (h+=3; *h && hw != nw; hw = hw<<8 | *++h);
	return *h ? (char *)h-3 : 0;
}

#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))

#define BITOP(a,b,op) \
 ((a)[(size_t)(b)/(8*sizeof *(a))] op (size_t)1<<((size_t)(b)%(8*sizeof *(a))))

static char *twoway_strstr(const unsigned char *h, const unsigned char *n)
{
	const unsigned char *z;
	size_t l, ip, jp, k, p, ms, p0, mem, mem0;
	size_t byteset[32 / sizeof(size_t)] = { 0 };
	size_t shift[256];

	/* Computing length of needle and fill shift table */
	for (l=0; n[l] && h[l]; l++)
		BITOP(byteset, n[l], |=), shift[n[l]] = l+1;
	if (n[l]) return 0; /* hit the end of h */

	/* Compute maximal suffix */
	ip = -1; jp = 0; k = p = 1;
	while (jp+k<l) {
		if (n[ip+k] == n[jp+k]) {
			if (k == p) {
				jp += p;
				k = 1;
			} else k++;
		} else if (n[ip+k] > n[jp+k]) {
			jp += k;
			k = 1;
			p = jp - ip;
		} else {
			ip = jp++;
			k = p = 1;
		}
	}
	ms = ip;
	p0 = p;

	/* And with the opposite comparison */
	ip = -1; jp = 0; k = p = 1;
	while (jp+k<l) {
		if (n[ip+k] == n[jp+k]) {
			if (k == p) {
				jp += p;
				k = 1;
			} else k++;
		} else if (n[ip+k] < n[jp+k]) {
			jp += k;
			k = 1;
			p = jp - ip;
		} else {
			ip = jp++;
			k = p = 1;
		}
	}
	if (ip+1 > ms+1) ms = ip;
	else p = p0;

	/* Periodic needle? */
	if (memcmp(n, n+p, ms+1)) {
		mem0 = 0;
		p = MAX(ms, l-ms-1) + 1;
	} else mem0 = l-p;
	mem = 0;

	/* Initialize incremental end-of-haystack pointer */
	z = h;

	/* Search loop */
	for (;;) {
		/* Update incremental end-of-haystack pointer */
		if (z-h < l) {
			/* Fast estimate for MAX(l,63) */
			size_t grow = l | 63;
			const unsigned char *z2 = memchr(z, 0, grow);
			if (z2) {
				z = z2;
				if (z-h < l) return 0;
			} else z += grow;
		}

		/* Check last byte first; advance by shift on mismatch */
		if (BITOP(byteset, h[l-1], &)) {
			k = l-shift[h[l-1]];
			if (k) {
				if (k < mem) k = mem;
				h += k;
				mem = 0;
				continue;
			}
		} else {
			h += l;
			mem = 0;
			continue;
		}

		/* Compare right half */
		for (k=MAX(ms+1,mem); n[k] && n[k] == h[k]; k++);
		if (n[k]) {
			h += k-ms;
			mem = 0;
			continue;
		}
		/* Compare left half */
		for (k=ms+1; k>mem && n[k-1] == h[k-1]; k--);
		if (k <= mem) return (char *)h;
		h += p;
		mem = mem0;
	}
}

char *musl_strstr(const char *h, const char *n)
{
	/* Return immediately on empty needle */
	if (!n[0]) return (char *)h;

	/* Use faster algorithms for short needles */
	h = strchr(h, *n);
	if (!h || !n[1]) return (char *)h;
	if (!h[1]) return 0;
	if (!n[2]) return twobyte_strstr((void *)h, (void *)n);
	if (!h[2]) return 0;
	if (!n[3]) return threebyte_strstr((void *)h, (void *)n);
	if (!h[3]) return 0;
	if (!n[4]) return fourbyte_strstr((void *)h, (void *)n);

	return twoway_strstr((void *)h, (void *)n);
}

////////////////////////////////////////////////////////////////////////////////
//- Program

static I64 rdtscp(void)
{
    Uz hi, lo;
    asm volatile ("rdtscp" : "=d"(hi), "=a"(lo) :: "cx", "memory");
    return (I64)hi<<32 | lo;
}

static void fill(char *buffer, Iz len, U64 seed) {
  U64 s = seed;
  for (Iz i = 0; i < len; i++) {
    s = s * 1111111111111111111u + 1;
    buffer[i] = 'A' + (char)(s >> 60);
    /* if (s & 1) buffer[i] |= 0x20; // to_lower */
  }
}


ptrdiff_t wrapped_strstr(const char *haystack, const char *needle) {
  char *p = strstr(haystack, needle);
  return p ? p - haystack : -1;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check_correctness() {
  enum {
    max_needle_len = 33,
  };

  U64 rng = 1;
  static char haystack[63];

  for (Iz haystack_len = 1; haystack_len <= countof(haystack); haystack_len++) {
    fill(haystack, haystack_len, (U64)&rng);
    haystack[haystack_len - 1] = '\0';

    static char needle[max_needle_len + 1] = {0};
    for (Iz needle_len = 1; needle_len <= max_needle_len; needle_len++) {
      for (Iz should_fill_needle_randomly = 0; should_fill_needle_randomly < 2; should_fill_needle_randomly++) {
        if (should_fill_needle_randomly) {
          fill(needle, needle_len, (U64)&rng + 5);
          needle[needle_len] = '\0';
        } else {
          memcpy(needle, haystack + countof(haystack) - 1 - needle_len, needle_len);
          needle[needle_len] = '\0';
        }

        ptrdiff_t actual = wrapped_strstr(haystack, needle);
        ptrdiff_t got = search_rabin_karp_avx2(haystack, strlen(haystack),
                                               needle, strlen(needle));
        if (got != actual) {
          printf("ERROR: Expected %ld but got %ld for:\nhaystack: %s\nneedle: %s\n",
                 actual, got, haystack, needle);
        }
      }
    }
  }
}

int main() {
  check_correctness();

  U64 rng = 1;
  static char haystack[1 << 25];
  fill(haystack, countof(haystack), (U64)&rng);

  enum {
    max_needle_len = 5,
    bench_n_samples = 1<<6,
  };

  static char needle[max_needle_len + 1] = {0};
  for (Iz needle_len = 2; needle_len <= max_needle_len; needle_len++) {
    fill(needle, needle_len, (U64)&rng + 5);
    needle[needle_len] = '\0';
    I64 best;
    Iz correct_ans;

    best = -1u>>1;
    for (int n = 0; n < bench_n_samples; n++) {
      I64 time = -rdtscp();
      correct_ans = wrapped_strstr(haystack, needle);
      volatile ptrdiff_t sink = correct_ans; (void)sink;
      time += rdtscp();
      best = best < time ? best : time;
    }
    printf("%-8s%3ld%10ld\n", "glibc", needle_len, best);

    best = -1u>>1;
    for (int n = 0; n < bench_n_samples; n++) {
      I64 time = -rdtscp();
      char *p = musl_strstr(haystack, needle);
      volatile char *sink = p; (void)sink;
      time += rdtscp();
      best = best < time ? best : time;
    }
    printf("%-8s%3ld%10ld\n", "musl", needle_len, best);

    best = -1u>>1;
    for (int n = 0; n < bench_n_samples; n++) {
      I64 time = -rdtscp();
      Iz got = search_rabin_karp(haystack, countof(haystack), needle, needle_len);
      volatile Iz sink = got; (void)sink;
      time += rdtscp();
      best = best < time ? best : time;
    }
    printf("%-8s%3ld%10ld\n", "rabin", needle_len, best);

    best = -1u>>1;
    for (int n = 0; n < bench_n_samples; n++) {
      I64 time = -rdtscp();
      Iz got = search_rabin_karp_avx2(haystack, countof(haystack), needle, needle_len);
      volatile Iz sink = got; (void)sink;
      time += rdtscp();
      best = best < time ? best : time;
    }
    printf("%-8s%3ld%10ld\n", "avx", needle_len, best);

    tassert(search_rabin_karp(haystack, countof(haystack), needle, needle_len) == correct_ans);
    tassert(search_rabin_karp_avx2(haystack, countof(haystack), needle, needle_len) == correct_ans);
    if (correct_ans < 0) printf("String %s not found\n", needle);
  }
  
  return 0;
}
