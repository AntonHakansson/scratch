#if IN_SHELL /* $ bash ear-clipping.c
cc ear-clipping.c -o ear-clipping              -fsanitize=undefined -Wall -g3 -O0 -lpthread -lraylib
cc ear-clipping.c -o app.so -DBUILD_RELOADABLE -fsanitize=undefined -Wall -g3 -O0 -shared -fPIC -lraylib
# cc ear-clipping.c -o ear-clipping -DAMALGAMATION -Wall -O3 -lpthread -lraylib
exit # */
#endif

// title: Ear clipping algorithm
// license: This is free and unencumbered software released into the public domain.

#include <raylib.h>

typedef unsigned char U8;
typedef unsigned long U32;
typedef unsigned long long U64;
typedef typeof((char *)0-(char *)0) Size;
typedef typeof(sizeof(0))           USize;

#define size_of(s)   (Size)sizeof(s)
#define count_of(s)  (size_of((s)) / size_of(*(s)))
#define assert(c)    while((!(c))) __builtin_trap()
#define new(a, t, n) ((t *) arena_alloc(a, size_of(t), (Size)_Alignof(t), (n)))

typedef struct { U8 *beg, *end; } Arena;

__attribute((malloc, alloc_size(2,4), alloc_align(3)))
static U8 *arena_alloc(Arena *a, Size objsize, Size align, Size count) {
  Size padding = -(USize)(a->beg) & (align - 1);
  Size total   = padding + objsize * count;
  if (total >= (a->end - a->beg)) {
		TraceLog(LOG_FATAL, "Out of memory.");
  }
  U8 *p = a->beg + padding;
  __builtin_memset(p, 0, objsize * count);
  a->beg += total;
  return p;
}

////////////////////////////////////////////////////////////////////////////////
//- Executable / Event loop

#if !defined(BUILD_RELOADABLE)

#if defined(AMALGAMATION)
	void *update(Arena *, Arena *, void *);
#else
  #define RTLD_NOW 0x2
	extern void *dlopen  (const char *__file, int __mode);
	extern int   dlclose (void *__handle);
	extern void *dlsym   (void *__restrict __handle, const char *__restrict __name);
#endif // AMALGAMATION

typedef struct {
  void *handle;
  void *(*update)(Arena *, Arena *, void *);
} AppCode;

static AppCode maybe_load_or_reload_app_code(AppCode app_code, _Bool should_reload)
{
  AppCode result = app_code;
  _Bool should_init = (app_code.handle == 0);

	if (should_reload) {
   #if defined(AMALGAMATION)
   #else
			assert(app_code.handle && "Can't reload unloaded executable.");
			void *dummy = app_code.handle;
			app_code.update(0, 0, dummy); // nofify pre-reload
			dlclose(app_code.handle);
   #endif
  }
  if (should_init || should_reload) {
    result = (AppCode){0};
		#if defined(AMALGAMATION)
				result.handle = update;
				result.update = update;
		#else
		  result.handle = dlopen("./app.so", RTLD_NOW);
		  assert(result.handle);
		  result.update = dlsym(result.handle, "update");
		  assert(result.update);
		#endif
  }

  return result;
}

int main(int argc, char **argv)
{
  Size HEAP_CAP = 1ll << 30;
  void *heap = MemAlloc(HEAP_CAP);

	Arena arena = (Arena){heap, heap + HEAP_CAP};
	Size frame_cap = 1ll << 26;
  Arena frame = {0};
  {
    frame.beg = new(&arena, U8, frame_cap);
		frame.end = frame.beg + frame_cap;
  }

	AppCode app_code = {0};
	void *app_state = 0;

	InitWindow(800,800, "Resolution-independent TTF font experiment(s)");
	SetWindowState(FLAG_WINDOW_RESIZABLE);
	while (!WindowShouldClose()) {
		app_code = maybe_load_or_reload_app_code(app_code, IsKeyReleased(KEY_R));
		app_state = app_code.update(&arena, &frame, app_state);
		frame.beg = frame.end - frame_cap; // rewind frame arena
	}
	CloseWindow();

  MemFree(heap);
  return 0;
}
#endif


////////////////////////////////////////////////////////////////////////////////
//- App Code

#if defined(AMALGAMATION) || defined(BUILD_RELOADABLE)

#define STATE_STRUCT_CAP (1 << 12)
#define MAX_POINTS (128)

typedef struct {
  Size struct_size;

  Arena *perm;
  Arena *frame;

  Vector2 ps[MAX_POINTS]; // == contour_ends[contour_count - 1]
  Size contour_ends[MAX_POINTS]; // last index + 1 of contour i
  Size contour_count;

} State;

State *p = 0;


static U32 random(U32 min, U32 max) {
  U32 range = (max - min);
  static U64 rng = 0x100;
  rng *= 1111111111111111111u;
  rng = rng ^ range;
  rng *= 1111111111111111111u;
  return (rng % range) + min;
}

#if 0
static U64 random_bounded(U64 range) {
  __uint128_t random64bit, multiresult;
  U64 leftover;
  U64 threshold;
  random64bit = rng(); // 64-bit random integer
  multiresult = random64bit * range;
  leftover = (U64)multiresult;
  if (leftover < range) {
    threshold = -range % range;
    while (leftover < threshold) {
      random64bit = rng();
      multiresult = random64bit * range;
      leftover = (U64)multiresult;
    }
  }
  return (U64)(multiresult >> 64); // [0, range)
}
#endif

static float randomf() {
  return (float)random(0, 1u << 31u) / (float)(1u << 31u);
}

static float clamp(float v, float min, float max) {
  if (v < min)
    return min;
  if (v > max)
    return max;
  return v;
}

#include <raymath.h>
static void generate_contour(State *p) {

  Size count = random(6, 24);
  for (Size i = 0; i < count; i++) {
    float x = cosf((i / (float)count) * 2 * M_PI - M_PI) * 0.5f + .5f;
    float y = sinf((i / (float)count) * 2 * M_PI - M_PI) * 0.5f + .5f;
		p->ps[i] = (Vector2){
      x * 0.9f + 0.04f + (randomf() * 2.f - 1.f) * 0.1f,
      clamp(y * 0.9f + (randomf() * 2.f - 1.f) * 0.1f, 0.001, 0.998)
    };
	}
  p->contour_ends[0] = count;
  p->contour_count = 1;

  TraceLog(LOG_INFO, "Generated %d Points", count);
}

static void generate_contour_with_holes(State *p) {
  Size contour_count = (Size)(randomf() * 1.999) + 1;
  contour_count = 2;
#if 1
  generate_contour(p);
  Size i = p->contour_ends[0];
  for (Size c = 1; c < contour_count; c++) {
    Size contour_end = i + random(6, 10);
    for (; i < contour_end; i++) {
      float radius = 1.f / (c + 2);
      float theta = (i / (float)contour_end);
			float x = -cosf(theta * 2 * M_PI - M_PI) * radius + .5f;
			float y = sinf(theta * 2 * M_PI - M_PI) * radius + .5f;
      p->ps[i] = (Vector2){ x * 0.8f, y * 0.8f + (randomf() * 2.f - 1.f) * 0.01f  };
		}
		p->contour_ends[c] = contour_end;
  }
#else
  Size i = 0;
  for (Size c = 0; c < contour_count; c++) {
    Size contour_end = i + random(6, 24);
    for (; i < contour_end; i++) {
      float radius = 1.f / (c + 2);
      float theta = (i / (float)contour_end);
			float x = cosf(theta * 2 * M_PI - M_PI) * radius + .5f;
			float y = sinf(theta * 2 * M_PI - M_PI) * radius + .5f;
			p->ps[i] = (Vector2){ x * 0.9f + 0.04f + (randomf() * 2.f - 1.f) * 0.1f, clamp(y * 0.9f + (randomf() * 2.f - 1.f) * 0.1f, 0.001, 0.999) };
		}
		p->contour_ends[c] = contour_end;
  }
#endif
  p->contour_count = contour_count;
  TraceLog(LOG_INFO, "Generated %d contours", contour_count);
}


typedef struct Triangle_List {
  Size *triangles;
  Size triangles_count;
} Triangle_List;

Triangle_List ear_clip(Arena *arena, Vector2 *ps, Size ps_count)
{
  Size *triangles = new(arena, Size, (ps_count - 2) * 3);
  Size triangles_count = 0;

  Arena temp = *arena;

  Size *index_list = new (&temp, Size, ps_count);
  Size count = ps_count;
	for (Size i = 0; i < ps_count; i++) {
    index_list[i] = i;
  }

  Size tries = 64; // REVIEW: hmm are we guaranteed to exit???
  for (Size i = 0; count > 2 && (tries--); i++) {

    i = (i < count) ? i : 0;
    Size cur = i;
    Size pl  = i - 1 < 0     ? count - 1 : i - 1;
    Size pr = i + 1 < count ? i + 1 : 0;

    cur = index_list[cur];
    pl = index_list[pl];
    pr = index_list[pr];

    Vector2 p0 = ps[pl];
		Vector2 p1 = ps[cur];
    Vector2 p2 = ps[pr];

    _Bool is_ear = 0;
    {
      Vector2 v = Vector2Subtract(p1, p0);
      Vector2 w = Vector2Subtract(p2, p0);
      float det = (v.x * w.y) - (w.x * v.y);
      is_ear = det > 0.f;
    }

    if (is_ear) {

      /* If the ear does not contain other points we can safely form a triangle, and */
      /* remove the "tip" of the ear from the working set. */

      _Bool ear_contains_other_points = 0;
      {
        for (Size j = 0; j < count; j++) {
          if (j == pl || j == cur || j == pr) { continue; }
					Vector2 other = ps[j];
					if (CheckCollisionPointTriangle(other, p0, p1, p2)) {
            ear_contains_other_points = 1;
						break;
          }
        }
      }

			if (!ear_contains_other_points) {
				triangles[triangles_count * 3 + 0] = pl;
				triangles[triangles_count * 3 + 1] = cur;
				triangles[triangles_count * 3 + 2] = pr;
        triangles_count++;

        // Remove tip
        for (Size k = i; k < count - 1; k++) {
          index_list[k] = index_list[k + 1];
        }
        count--;
      }
    }
  }

#if 0
  for (Size t = 0; t < triangles_count; t++) {
    Vector2 p0 = ps[triangles[t*3 + 0]];
    Vector2 p1 = ps[triangles[t*3 + 1]];
    Vector2 p2 = ps[triangles[t*3 + 2]];
    Color c = ColorFromHSV(((float)t / (float)triangles_count) * 360.f, 1.0f, 1.0f);
    DrawTriangle(p1, p0, p2, c);
  }
#endif

  return (Triangle_List){ triangles, triangles_count };
}

/* Assumes first contour is exterior (CW winding), subsequent contours are holes(CCW winding)  */
Triangle_List ear_clip_extended(Arena *arena, Vector2 *ps, Size *contour_ends, Size contour_count) {
  assert(contour_count >= 1);

  Size *triangles = new(arena, Size, ((contour_ends[contour_count - 1])) * 3);
  Size triangles_count = 0;

  Arena temp = *arena;

  typedef struct { Size from; Size to; } Bridge;
  Bridge bridges[MAX_POINTS] = {0};

  /* The extended ear clipping algorithm transform the  */
  /* The working set has to be constructed such that we form a single contingent simple */
  /* polygon connected by edges/bridges between the hole(s) and the exterior. To find such edges we */
  /* must find the so called MIP triangle. */

  for (Size contour_idx = 1; contour_idx < contour_count; contour_idx++) {
    Size contour_begin = contour_ends[contour_idx - 1];

    Size M_i = -1;
    Vector2 M = {0};
    {
      float best = 0.0f;
			for (Size i = contour_begin; i < contour_ends[contour_idx]; i++) {
				float x = ps[i].x;
				if (x > best) {
					best = x;
					M = ps[i];
          M_i = i;
				}
			}
    }

    Vector2 I = {0};
    Vector2 P = {0};
    Size P_i = -1;
    {
      Vector2 p0 = ps[contour_ends[0] - 1], p1 = {0};
      for (Size j = 0; j < contour_ends[0]; j++, p0 = ps[j - 1]) {
        p1 = ps[j];

        if (!(p0.x > M.x || p1.x > M.x))
          continue;
        if (!((p0.y > M.y) ^ (p1.y > M.y)))
          continue;

        /*
					Line segment: p0 + s*t where s = normal(p1-p0)  and \( t \in [0, 1] \)
					Ray: M + {u, 0} where \( u > 0 \)

					They intersect when equal.

					(p0 + s*t).y > M.y
					s*t > M - p0

				 */
        Vector2 to_p1 = Vector2Normalize(Vector2Subtract(p1, p0));
        if (__builtin_fabsf(to_p1.y) < 1e-3 ) continue;
        float t = ((M.y - p0.y) / to_p1.y);
        if (t > 0 && t < 1.f) {
          I = Vector2Add(p0, Vector2Scale(to_p1, t));
#if 0
          // According to the reference paper, they make P the next point but with my
          // implementation the next point works better.
					P = p1;
          P_i = j;
#else
          P = p0;
          P_i = j - 1 < 0 ? contour_ends[0] - 1: j-1;
#endif

          // TODO check that M and P are visible / unobstructed

					break;
        }
      }
    }

    if (P_i >= 0) {
#if 1
			float debug_radius = 0.008f;
			DrawLineV(M, I, BLUE);
			DrawLineV(M, P, MAGENTA);
			DrawCircleV(M, debug_radius, BLUE);
			DrawCircleV(I, debug_radius, GREEN);
			DrawCircleV(P, debug_radius, MAGENTA);
#endif
    }

    bridges[contour_idx] = (Bridge){P_i, M_i};
  }

  Size *index_list = new(&temp, Size, MAX_POINTS);
  Size count = 0;
  {
		for (Size i = 0; i < contour_ends[0]; i++) {

      Size insert_bridge_to_contour = -1;
			for (Size hole = 1; hole < contour_count; hole++) {
				if (i == bridges[hole].from) {
          insert_bridge_to_contour = hole;
					break;
				}
			}
      if (insert_bridge_to_contour > 0) {
			  index_list[count++] = i;
        Size beg = contour_ends[insert_bridge_to_contour - 1];
        Size end = contour_ends[insert_bridge_to_contour - 0];
        Size hole_count = end - beg + 1;
        Size at = bridges[insert_bridge_to_contour].to;
        while (hole_count--) {
			    index_list[count++] = at++;
          if (at >= end) at = beg;
        }
			  index_list[count++] = i;
			} else {
			  index_list[count++] = i;
      }
		}
  }

#if 1
  {
    Size i = 0;
    U8 *seen = new(&temp, U8, count);
    for (Size i = 0; i < count; i++) {
      char buf[256];
      __builtin_snprintf(buf, sizeof buf, "p%ld", i);
      Vector2 pos = Vector2Add(p->ps[index_list[i]], (Vector2){-0.03 + seen[index_list[i]] * 0.03f, 0});
      DrawTextEx(GetFontDefault(), buf, pos, 0.02f, 0.001f, BLUE);
      seen[index_list[i]] += 1;
    }
  }
#endif

  Size tries = 128; // REVIEW: hmm are we guaranteed to exit???
  for (Size i = 0; count > 2 && (tries--); i++) {

    i = (i < count) ? i : 0;
    Size cur = i;
    Size pl  = i - 1 < 0     ? count - 1 : i - 1;
    Size pr = i + 1 < count ? i + 1 : 0;

    cur = index_list[cur];
    pl = index_list[pl];
    pr = index_list[pr];

    Vector2 p0 = ps[pl];
		Vector2 p1 = ps[cur];
    Vector2 p2 = ps[pr];

    _Bool is_ear = 0;
    {
      Vector2 v = Vector2Subtract(p1, p0);
      Vector2 w = Vector2Subtract(p2, p0);
      float det = (v.x * w.y) - (w.x * v.y);
      is_ear = det > 0.f; // CW
    }

    if (is_ear) {
      _Bool ear_contains_other_points = 0;
      {
        for (Size j = 0; j < count; j++) {
          if (j == pl || j == cur || j == pr) { continue; }
					Vector2 other = ps[index_list[j]];
					if (CheckCollisionPointTriangle(other, p0, p1, p2)) {
            ear_contains_other_points = 1;
						break;
          }
        }
      }

			if (!ear_contains_other_points) {
				triangles[triangles_count * 3 + 0] = pl;
				triangles[triangles_count * 3 + 1] = cur;
				triangles[triangles_count * 3 + 2] = pr;
        triangles_count++;

        // Remove vertex i from working set
        for (Size k = i; k < count - 1; k++) {
          index_list[k] = index_list[k + 1];
        }
        count--;
      }
    }
  }

  if (tries == 0) {
    // TODO error.
    TraceLog(LOG_WARNING, "Exceeded number of tries.");
  }

#if 1
  for (Size t = 0; t < triangles_count; t++) {
    Vector2 p0 = ps[triangles[t*3 + 0]];
    Vector2 p1 = ps[triangles[t*3 + 1]];
    Vector2 p2 = ps[triangles[t*3 + 2]];
    Color c =
      ColorFromHSV(((float)t / (float)triangles_count) * 360.f, 1.0f, 1.0f);
    c = ColorAlpha(c, 0.5f);
    /* DrawTriangleLines(p1, p0, p2, c); */
    DrawTriangle(p1, p0, p2, c);
  }
#endif

  return (Triangle_List){ triangles, triangles_count };
}

////////////////////////////////
//- List macros
#define CheckNil(nil,p) ((p) == 0 || (p) == nil)
#define SetNil(nil,p) ((p) = nil)

//-- Base Doubly-Linked-List Macros
#define DLLInsert_NPZ(nil,f,l,p,n,next,prev) (CheckNil(nil,f) ? \
((f) = (l) = (n), SetNil(nil,(n)->next), SetNil(nil,(n)->prev)) :\
CheckNil(nil,p) ? \
((n)->next = (f), (f)->prev = (n), (f) = (n), SetNil(nil,(n)->prev)) :\
((p)==(l)) ? \
((l)->next = (n), (n)->prev = (l), (l) = (n), SetNil(nil, (n)->next)) :\
(((!CheckNil(nil,p) && CheckNil(nil,(p)->next)) ? (0) : ((p)->next->prev = (n))), ((n)->next = (p)->next), ((p)->next = (n)), ((n)->prev = (p))))
#define DLLPushBack_NPZ(nil,f,l,n,next,prev) DLLInsert_NPZ(nil,f,l,l,n,next,prev)
#define DLLPushFront_NPZ(nil,f,l,n,next,prev) DLLInsert_NPZ(nil,l,f,f,n,prev,next)
#define DLLRemove_NPZ(nil,f,l,n,next,prev) (((n) == (f) ? (f) = (n)->next : (0)),\
((n) == (l) ? (l) = (l)->prev : (0)),\
(CheckNil(nil,(n)->prev) ? (0) :\
((n)->prev->next = (n)->next)),\
(CheckNil(nil,(n)->next) ? (0) :\
((n)->next->prev = (n)->prev)))

enum NodeFlags {
  NodeFlags_CCW, // counter clockwise means that the contour is a hole
  NodeFlags_EarTip,
};
typedef struct Node Node;
struct Node {
  Node *next;
  Node *prev;
  U32 flags;
  Vector2 pos;
};

typedef struct Contour Contour;
struct Contour {
  U32 flags;
  Node *first;
  Node *last;
  Node *next;
};

static Triangle_List ear_clip_extended_ll(Arena *arena, Vector2 *ps, Size *contour_ends,
                                 Size contour_count) {
  assert(contour_count >= 1);

  Size *triangles = new(arena, Size, ((contour_ends[contour_count - 1])) * 3);
  Size triangles_count = 0;

  Arena temp = *arena;
}

#define min(a, b) ((a) < (b)) ? (a) : (b)

void *update(Arena *perm, Arena *frame, void *pstate) {
  if (pstate == 0) { // Init
    p = (State *) arena_alloc(perm, STATE_STRUCT_CAP, _Alignof(State), 1);
    p->struct_size = size_of(*p);
    p->perm = perm;
    p->frame = frame;

    generate_contour(p);
  }
  if (perm == 0 && frame == 0) { // Pre-reload
    TraceLog(LOG_INFO, "Reload.");
    return p;
  }
  if (p == 0) {    // Post-reload
    State *prev_p = (State *)pstate;
    if (prev_p->struct_size != size_of(*p)) {
      TraceLog(LOG_INFO, "Resized State schema %ld -> %ld", prev_p->struct_size, size_of(*p));
    }
    p = prev_p;
    p->struct_size = size_of(*p);
  }

  if (IsKeyPressed(KEY_SPACE)) {
    generate_contour_with_holes(p);
  }

  BeginDrawing();
  ClearBackground(BLACK);

  Camera2D camera = {
    .zoom = min(GetRenderWidth(), GetRenderHeight()),
  };
  BeginMode2D(camera);
  Rectangle boundary = {0.f, 0.f, 1.f, 1.f};
  DrawRectangleLinesEx(boundary, 0.008f, GREEN);

  if (p->contour_count > 0) {
    if (0) {
      Triangle_List tri = ear_clip(p->frame, p->ps, p->contour_ends[0]);
      for (Size t = 0; t < tri.triangles_count; t++) {
        Vector2 p0 = p->ps[tri.triangles[t * 3 + 0]];
        Vector2 p1 = p->ps[tri.triangles[t * 3 + 1]];
				Vector2 p2 = p->ps[tri.triangles[t * 3 + 2]];
				Color c = ColorFromHSV(((float)t / (float)tri.triangles_count) * 360.f, .1f, .9f);
				/* DrawTriangleLines(p1, p0, p2, c); */
				/* DrawTriangle(p1, p0, p2, c); */
      }
    }

    if (0) {
      Size contour_begin = 0;
      for (Size c = 0; c < p->contour_count; c++, contour_begin = p->contour_ends[c - 1]) {
        for (Size i = contour_begin; i < p->contour_ends[c]; i++) {
				  Color col_contour = ColorFromHSV(((float)c / (float)p->contour_count) * 360.f, .9f, .9f);
				  Color col_idx = ColorFromHSV(((float)i / (float)p->contour_ends[c]) * 360.f, .9f, .9f);
          /* DrawCircleV(p->ps[i], 0.008f, col_idx); */
          char buf[256];
          __builtin_snprintf(buf, sizeof buf, "p%ld", i);
          /* DrawTextEx(GetFontDefault(), buf, p->ps[i], 0.02f, 0.001f, col_contour); */
        }

				Vector2 prev = p->ps[p->contour_ends[c] - 1];
				for (Size i = contour_begin; i < p->contour_ends[c]; i++) {
					// DrawLineEx(prev, p->ps[i], 0.002f, GRAY);
					prev = p->ps[i];
        }
      }
    }

    if (1) {
			ear_clip_extended(p->frame, p->ps, p->contour_ends, p->contour_count);
    }

	}

  EndMode2D();
  EndDrawing();

  return p;
}

#endif
