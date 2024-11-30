#if IN_SHELL /* $ bash pendulum.c
cc pendulum.c -o pendulum    -fsanitize=undefined -Wall -g3 -O0 -lpthread -lraylib
cc pendulum.c -o pendulum.so -DBUILD_RELOADABLE -fsanitize=undefined -Wall -g3 -O0 -shared -fPIC -lraylib -Wno-unused-function
# cc pendulum.c -o pendulum    -DAMALGAMATION -Wall -O3 -lpthread -lraylib
exit # */
#endif

// title: Experimental, Immediate-mode Animation System
// license: This is free and unencumbered software released into the public domain.

#include <stdio.h>
#include <raylib.h>
#include <raymath.h>

typedef unsigned char U8;
typedef unsigned long U32;
typedef unsigned long long U64;
typedef          long long I64;
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
//- Executable <-> App interface

typedef struct {
  Arena *perm;
  Arena *frame;
  Arena *read_only;
} App_Update_Params;

////////////////////////////////////////////////////////////////////////////////
//- Executable / Event loop

#if !defined(BUILD_RELOADABLE)

#if defined(AMALGAMATION)
	void *update(Arena *, Arena *, void *);
#else
  extern void perror(const char *s);

  #define RTLD_NOW 0x2
	extern void *dlopen  (const char *__file, int __mode);
	extern int   dlclose (void *__handle);
	extern void *dlsym   (void *__restrict __handle, const char *__restrict __name);
#endif // AMALGAMATION

typedef struct {
  void *handle;
  void *(*update)(App_Update_Params, void *);
  long last_modified;
} AppCode;

static AppCode maybe_load_or_reload_app_code(AppCode app_code, _Bool should_reload) {
  AppCode result = app_code;
  _Bool should_init = (app_code.handle == 0);

  #if !defined(AMALGAMATION)
  long modtime = GetFileModTime("./pendulum.so");
  if (app_code.handle) {
    if (app_code.last_modified != modtime) {
      should_reload |= 1;
    }
  }
  #endif

	if (should_reload) {
   #if defined(AMALGAMATION)
   #else
			assert(app_code.handle && "Can't reload unloaded executable.");
			void *dummy = app_code.handle;
			app_code.update((App_Update_Params){}, dummy); // nofify pre-reload
			dlclose(app_code.handle);
   #endif
  }
  if (should_init || should_reload) {
    result.handle = 0;
    result.update = 0;
		#if defined(AMALGAMATION)
				result.handle = update;
				result.update = update;
    #else
		  result.handle = dlopen("./pendulum.so", RTLD_NOW);
      if (result.handle) {
        result.update = dlsym(result.handle, "update");
		    assert(result.update);
        result.last_modified = modtime;
      }
      else {
        perror("dlopen");
      }
    #endif
  }

  return result;
}

int main(int argc, char **argv) {
  Size HEAP_CAP = 1ll << 30;
  void *heap = MemAlloc(HEAP_CAP);

	Arena arena = (Arena){heap, heap + HEAP_CAP};
  Arena frame = {0};
  {
	  Size frame_cap = 1ll << 26;
    frame.beg = new(&arena, U8, frame_cap);
		frame.end = frame.beg + frame_cap;
  }

	AppCode app_code = {0};
	void *app_state = 0;

  SetConfigFlags(FLAG_MSAA_4X_HINT);
	InitWindow(800,800, "Pendulum");
	SetWindowState(FLAG_WINDOW_RESIZABLE);
  while (!WindowShouldClose()) {
    Arena frame_reset = frame;
    app_code = maybe_load_or_reload_app_code(app_code, IsKeyReleased(KEY_R));
    if (app_code.update) {
      app_state = app_code.update(
          (App_Update_Params){
              .perm = &arena,
              .frame = &frame,
          }, app_state);
    }
    frame = frame_reset;
	}
	CloseWindow();

  MemFree(heap);
  return 0;
}
#endif


////////////////////////////////
// List macros
#define CheckNil(nil,p) ((p) == 0 || (p) == nil)
#define SetNil(nil,p) ((p) = nil)

//- rjf: Base Doubly-Linked-List Macros
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


////////////////////////////////////////////////////////////////////////////////
//- App Code

#if defined(AMALGAMATION) || defined(BUILD_RELOADABLE)

#define MAX_STATE_CAP (1 << 12)
#define min(a, b) ((a) < (b)) ? (a) : (b)


typedef struct Pendulum Pendulum;
struct Pendulum {
  float mass;
  float length;
  Vector2 fixture;
  Vector2 pos;
  Vector2 vel;
};

typedef struct {
  Size struct_size;

  Arena *perm;
  Arena *frame;

  Pendulum *pendulum_array;
  Size pendulum_max_count;
  Size pendulum_count;
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

static float randomf() {
  return (float)random(0, 1u << 31u) / (float)(1u << 31u);
}

static float clamp(float v, float min, float max) {
  if (v < min) return min;
  if (v > max) return max;
  return v;
}

static float exp_decay(float a, float b, float decay, float dt) {
  return b + (a - b) * exp(-decay * dt);
}

static Vector2 exp_decay_v(Vector2 a, Vector2 b, float decay, float dt) {
  Vector2 r = {};
  r.x = exp_decay(a.x, b.x, decay, dt);
  r.y = exp_decay(a.y, b.y, decay, dt);
  return r;
}

U64 hash_fnv1a(void *buf, U32 len) {
  U64 hash = 0xcbf29ce484222325;
  while (--len) {
    hash ^= *(unsigned char*)buf;
    hash *= 0x00000100000001b3;
    buf++;
  }
  hash ^= hash >> 32;
  return hash;
}

void draw_fixture(Vector2 fixture) {
  Vector2 dim = (Vector2){100, 20};
  Vector2 position = (Vector2){fixture.x - dim.x / 2, fixture.y};
  Color color = WHITE;

  { // draw diagonal line on fixture base
    float gap = 10.f;
    Size steps = dim.x / gap;
    float xoff = (gap * (steps + 1) - dim.x) * 0.7f;
    for (Size step = 0; step < steps; step++) {
      Vector2 on_line = Vector2Add(position, (Vector2){xoff + step * gap, 0});
      Vector2 above_line = Vector2Add(on_line, (Vector2){-10.f, -10.f});
      DrawLineV(on_line, above_line, ColorBrightness(color, -0.4f));
    }
  }
  // DrawLineV(fixture, Vector2Add(fixture, (Vector2){0, 10.f}), RED);
  DrawLineV(position, (Vector2){position.x + dim.x, position.y}, color);
}

void draw_spring_v(Vector2 start, Vector2 end, Color color) {
  float dim = 10.f;

  Vector2 dir =
    Vector2Normalize(Vector2Subtract(end, start));
  float distance =
    Vector2Length(Vector2Subtract(end, start));

  // DrawCircleSectorLines(Vector2 center, float radius, float startAngle, float
  // endAngle, int segments, Color color); // Draw circle sector outline
  // DrawCircleSectorLines();

  float spring_len = distance * 0.5f;
  float t_start = 15.f;
  float t_end = t_start + spring_len;

  Size count = 6;
  float t_step = spring_len / (count);

  Vector2 spring_start = Vector2Add(start, Vector2Scale(dir, t_start));
  Vector2 spring_end = Vector2Add(spring_start, Vector2Scale(dir, t_step * (count)));

  Vector2 pos = spring_start;
  for (Size step = 0; step < count; step++) {
    /* DrawCircleV(pos, 2.f, RED); */
    Vector2 tangent = (Vector2){ dir.y, -dir.x };
    Vector2 next_pos = Vector2Add(pos, Vector2Scale(dir, t_step));
    Vector2 c1 = Vector2Add(Vector2Scale(tangent, dim * 2), Vector2Lerp(pos, next_pos, 0.5f));
    Vector2 c2 = Vector2Add(Vector2Scale(tangent, dim), Vector2Lerp(pos, next_pos, 0.6f));
    DrawSplineSegmentBezierCubic(pos, c1, c2, next_pos, 1.f, color);
    pos = next_pos;
  }
  DrawLineV(start, spring_start, color);
  DrawLineV(spring_end, end, color);

  /* DrawCircleV(Vector2Add(start, Vector2Scale(dir, t_start)), 2.f, BLUE); */
  /* DrawCircleV(Vector2Add(start, Vector2Scale(dir, t_end)), 2.f, BLUE); */
}

void *update(App_Update_Params params, void *pstate) {
  if (pstate == 0) { // Init
    p = (State *) arena_alloc(params.perm, MAX_STATE_CAP, _Alignof(State), 1);
    p->struct_size = size_of(*p);
    p->perm = params.perm;
    p->frame = params.frame;

    p->pendulum_max_count = 128;
    p->pendulum_array = new (p->perm, Pendulum, p->pendulum_max_count);
    p->pendulum_count = 1;
  }
  if (params.perm == 0 && params.frame == 0) { // Pre-reload
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
  assert(p);

  BeginDrawing();
  ClearBackground(BLACK);

  Camera2D camera = { .zoom = 1, };
  camera.offset = (Vector2) { GetRenderWidth() / 2.f, GetRenderHeight() / 2.f };
  BeginMode2D(camera);

  if (IsKeyPressed(KEY_W)) {
    p->pendulum_count++;
  }
  if (IsKeyPressed(KEY_Q)) {
    p->pendulum_count--;
  }
  p->pendulum_count = clamp(p->pendulum_count, 0, p->pendulum_max_count);

  for (Size i = 0; i < p->pendulum_count; i++) {
    Pendulum *pendulum = &p->pendulum_array[i];
    if (i > 0) { pendulum->fixture = p->pendulum_array[i - 1].pos; }

    if (IsKeyPressed(KEY_SPACE)) {
      __builtin_memset(pendulum, 0, sizeof *pendulum);
    }
    if (pendulum->mass == 0) {
      pendulum->mass = 64.f;
      pendulum->length = 150;
      float theta = randomf() * 2 * 3.14;
      pendulum->pos = Vector2Scale( Vector2Rotate((Vector2){ 1.f, 0.f }, theta), pendulum->length);
    }
    float dt = GetFrameTime();

    draw_spring_v(pendulum->fixture, pendulum->pos, WHITE);
    if (i == 0) { draw_fixture(pendulum->fixture); }
    DrawCircleV(pendulum->pos, 16.f, RED);

    Vector2 dir = Vector2Normalize(Vector2Subtract(pendulum->pos, pendulum->fixture));
    float fg = pendulum->mass * 10.f;
    float stiffness = 1.0f * 64.f;
    float elongation =
        pendulum->length -
        Vector2Length(Vector2Subtract(pendulum->fixture, pendulum->pos));
    /* elongation = elongation > 0 ? 0 : elongation; */
    Vector2 force_radial = Vector2Scale(dir, stiffness * elongation);
    Vector2 force_gravity = {0, fg};

    Vector2 force = Vector2Add(force_gravity, force_radial);
    pendulum->vel = Vector2Add(pendulum->vel, Vector2Scale(force, dt));
    pendulum->pos = Vector2Add(pendulum->pos, Vector2Scale(pendulum->vel, dt));

    /* Vector2 correction = Vector2Subtract(Vector2Scale(Vector2Normalize(Vector2Subtract(pendulum->pos, pendulum->fixture)), pendulum->length), pendulum->pos); */
    /* pendulum->pos = Vector2Add(pendulum->pos, correction); */
  }

  EndMode2D();

  EndDrawing();

  return p;
}

#endif
