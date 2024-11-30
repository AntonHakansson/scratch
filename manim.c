#if IN_SHELL /* $ bash manim.c
cc manim.c -o manim    -fsanitize=undefined -Wall -g3 -O0 -lpthread -lraylib
cc manim.c -o manim.so -DBUILD_RELOADABLE -fsanitize=undefined -Wall -g3 -O0 -shared -fPIC -lraylib -Wno-unused-function
# cc manim.c -o manim    -DAMALGAMATION -Wall -O3 -lpthread -lraylib
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
} AppCode;

static AppCode maybe_load_or_reload_app_code(AppCode app_code, _Bool should_reload) {
  AppCode result = app_code;
  _Bool should_init = (app_code.handle == 0);

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
    result = (AppCode){0};
		#if defined(AMALGAMATION)
				result.handle = update;
				result.update = update;
		#else
		  result.handle = dlopen("./manim.so", RTLD_NOW);
      if (result.handle) {
		    result.update = dlsym(result.handle, "update");
		    assert(result.update);
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
	InitWindow(800,800, "Resolution-independent TTF font experiment(s)");
	SetWindowState(FLAG_WINDOW_RESIZABLE);
  while (!WindowShouldClose()) {
    Arena frame_reset = frame;
		app_code = maybe_load_or_reload_app_code(app_code, IsKeyReleased(KEY_R));
    app_state = app_code.update((App_Update_Params){
      .perm = &arena,
      .frame = &frame,
    }, app_state);
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

enum An_Shape_Kind {
  An_Shape_Kind_Unknown,
  An_Shape_Kind_Circle,
  An_Shape_Kind_Bezier,
  An_Shape_Kind_Text,
};

enum An_Animation_Kind {
  An_Animation_Kind_Unknown,
  An_Animation_Kind_Play,
  An_Animation_Kind_Wait,
  An_Animation_Kind_Interpolate,

  An_Animation_Kind_Translate,
  An_Animation_Kind_Fade_in,
  An_Animation_Kind_Fade_out,
};

typedef struct {
  enum An_Shape_Kind kind;

  U64 hash_key, last_touched_frame;
  Vector2 pos;

  _Bool hide;
  float radius;
  Color fill_color;
  float fill_alpha;
  Color border_color;
  float border_alpha;
} An_Shape;

typedef struct An_Animation {
  //- persistent data
  U64 hash_key;
  U64 last_touched_frame;
  _Bool first_frame; // Animation was added this frame.

  //- per-build links
  struct An_Animation *next;
  struct An_Animation *prev;
  struct An_Animation *first;
  struct An_Animation *last;
  struct An_Animation *parent;
  Size child_count;

  //- per-build data
  enum An_Animation_Kind kind;
  float duration;
  float stagger; // scheduler
  An_Shape *shape;
  float    *interpolate;
  Vector2  *interpolate_v;
  Vector2   target_pos;
  float target_radius;

  //- per-build artifacts
  float beg;
  float exp_decay_t;
} An_Animation;

void an_push_child(An_Animation *parent, An_Animation *node) {
  parent->child_count++;
  DLLPushBack_NPZ(0, parent->first, parent->last, node, next, prev);
  node->parent = parent;
}

typedef struct An_Animation_Rec {
  An_Animation *next;
  Size push_count;
  Size pop_count;
} An_Animation_Rec;

static An_Animation_Rec an_rec_df(An_Animation *an, An_Animation *root) {
  An_Animation_Rec r = {0};
  r.next = 0;
  if (!CheckNil(0, an->first)) {
    r.next = an->first;
    r.push_count += 1;
  } else {
    for (An_Animation *p = an; !CheckNil(0, p) && p != root; p = p->parent) {
      if (!CheckNil(0, p->next)) {
        r.next = p->next;
        break;
      }
      r.pop_count += 1;
    }
  }
  return r;
}

typedef struct An_Scheduler_Node An_Scheduler_Node;
struct An_Scheduler_Node {
  An_Animation *node;
  An_Scheduler_Node *next;
};

#define PERSIST_STORE_EXP (10)
#define AN_PLAYER_FLAG_PAUSED (1 << 0)
typedef struct An_Player {
  U64 flags;
  float t, dt, duration, speed_multiplier_exp;
  I64 frame;

  An_Shape     shape_store[1 << PERSIST_STORE_EXP];
  An_Animation persist_store[1 << PERSIST_STORE_EXP];

  float dummy_interpolate;
  Vector2 dummy_interpolate_v;

  An_Animation *root;

  An_Scheduler_Node *scheduler;
} An_Player;

void an_play_ex(An_Player *player, An_Animation *node) {
  assert(player->scheduler);
  an_push_child(player->scheduler->node, node);
}

typedef struct {
  Size struct_size;

  Arena *perm;
  Arena *frame;

  An_Player player;
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

#define FUNLINE_HASH (hash_fnv1a((char *)__FUNCTION__, count_of(__FUNCTION__)) + __LINE__)
#define an_circle(center, radius) an_circle_ex((&p->player), (FUNLINE_HASH), (center), (radius))
#define an_wait(duration) an_wait_ex((&p->player), (FUNLINE_HASH), (duration))
#define an_translate(shape, duration, pos) an_translate_ex((&p->player), (FUNLINE_HASH), (duration), (shape), (pos))
#define an_fade_in(shape, duration, radius) an_fade_in_ex((&p->player), (FUNLINE_HASH), (duration), (shape), (radius))
#define an_play(an) an_play_ex((&p->player), (an))

U64 an_h(U64 hash, U64 idx) {
  U64 mask = (1u << PERSIST_STORE_EXP) - 1;
  U64 step = (hash >> 60u) | 1;
  return (idx + step) & mask;
}

An_Animation *an_push_scheduler(An_Player *player, U64 hash_key, float stagger);
void an_pop_scheduler(An_Player *player);

void an_beg(An_Player *player, float dt) {
  player->frame += 1;
  player->dt = dt;
  player->t += dt;
  player->duration = 0.f;
  player->root = an_push_scheduler(player, (FUNLINE_HASH), 0);
}

void an_end(An_Player *player, float w, float h) {
  an_pop_scheduler(player);

  float hw = w / 2;
  float hh = h / 2;

  // update animations
  {
    An_Animation_Rec df = {.next = player->root};
    while ((df = an_rec_df(df.next, player->root)).next != 0) {
      An_Animation *cur = df.next;
      float end = cur->beg + cur->duration;

      _Bool is_playing = player->t >= cur->beg && player->t <= end;
      if (!is_playing) continue;

      /* float linear_t = (player->t - cur->beg) / cur->duration; */
      cur->exp_decay_t = exp_decay(cur->exp_decay_t, 1.f, 10, player->dt);

      switch (cur->kind) {
      case An_Animation_Kind_Interpolate: {
        if (cur->shape) { cur->shape->hide = 0; }
        *cur->interpolate   = exp_decay(*cur->interpolate, cur->target_radius, 5.f, player->dt);
        *cur->interpolate_v = exp_decay_v(*cur->interpolate_v, cur->target_pos, 10.f, player->dt);
      } break;
      };
    }
  }

  // Debug animation tree
  if (1) {
    Arena temp = *p->frame;

    Vector2 *stack = new(&temp, Vector2, 512);
    Size stack_count = 0;
    stack[stack_count++] = (Vector2){.x = -2.f, .y = -5.f, };
    float xoff = 0;

    An_Animation_Rec df = { .next = player->root, };
    for (; !CheckNil(0, df.next) && stack_count < 512; df = an_rec_df(df.next, player->root)) {

      Vector2 parent = stack[stack_count - 1];
      for (Size i = 0; i < df.push_count; i++) {
        Vector2 newparent = parent;
        newparent.y += 1.f;
        newparent.x -= 1.f;
        stack[stack_count++] = newparent;
        xoff = 0;
      }
      for (Size i = 0; i < df.pop_count; i++) {
        stack_count--;
        xoff = 4.f;
      }
      _Bool sibling = (df.push_count == 0 && df.pop_count == 0);
      if (sibling) { xoff += 2.f; }

      Vector2 node = stack[stack_count - 1];
      node.x += xoff;

      char tmp[32];
      snprintf(tmp, count_of(tmp), "%ld %ld", df.push_count, df.pop_count);
      DrawTextEx(GetFontDefault(), tmp, node, 1.f, 0.f, WHITE);

      float linear_t = (player->t - df.next->beg) / df.next->duration;
      linear_t = fmin(linear_t, 1.f);
      float width = 1.f;
      DrawRectangleV(node, (Vector2){width, .1f}, RED);
      DrawRectangleV(node, (Vector2){width * linear_t, .1f}, WHITE);
    }
  }

  // draw shapes
  {
    // TODO: linked list of shapes
    for (Size i = 0; i < 1 << PERSIST_STORE_EXP; i++) {
      An_Shape *shape = player->shape_store + i;
      if (shape->hash_key && !shape->hide) {
        Color color = shape->fill_color;
        DrawCircleLinesV(shape->pos, shape->radius, color);
      }
    }
  }

  { // render timeline
    float t = player->duration <= 0 ? 0 : player->t / player->duration;

    float timeline_height = 0.5f;
    float timeline_y = hh - timeline_height;
    Rectangle timeline_rect = { .x = -hw + 0,     .y = timeline_y, .width = w,                      .height = timeline_height, };
    Rectangle timeline_mark = { .x = -hw + t * w, .y = timeline_y, .width = timeline_height * 0.1f, .height = timeline_height, };
    DrawRectangleRec(timeline_rect, GRAY);
    DrawRectangleRec(timeline_mark, WHITE);
  }

  // restart
  if (player->t >= player->duration) {
    __builtin_memset(player, 0, sizeof(*player));
  }
}

An_Shape *an_circle_ex(An_Player *player, U64 hash_key, Vector2 center, float radius) {
  An_Shape *r = 0;

  U64 h = hash_key;
  for (Size i = h;;) {
    U64 idx = an_h(h, i);
    An_Shape *candidate = &player->shape_store[idx];
    if (candidate->hash_key == 0) {
      r = candidate;
      r->hash_key = hash_key;
      r->kind = An_Shape_Kind_Circle;
      r->pos = center;
      r->radius = radius;
      r->fill_color = WHITE;
      break;
    }
    if (candidate->hash_key == hash_key) {
      r = candidate;
      break;
    }
  }

  assert(r);
  r->last_touched_frame = player->frame;
  return r;
}

An_Animation *an_get_persistent_data(An_Player *player, U64 hash_key) {
  An_Animation *r = 0;
  U64 h = hash_key;
  for (Size i = h;;) {
    U64 idx = an_h(h, i);
    An_Animation *candidate = &player->persist_store[idx];
    if (candidate->hash_key == 0) {
      r = candidate;
      r->parent = r->next = r->prev = r->first = r->last = 0;
      r->hash_key = hash_key;
      r->interpolate = &player->dummy_interpolate;
      r->interpolate_v = &player->dummy_interpolate_v;
      r->first_frame = 1;
      break;
    }
    else if (candidate->hash_key == hash_key) {
      r = candidate;
      r->first_frame = 0;
      break;
    }
  }
  return r;
}

An_Animation *an_wait_ex(An_Player *player, U64 hash_key, float duration) {
  An_Animation *r = an_get_persistent_data(player, hash_key);
  r->kind = An_Animation_Kind_Interpolate;
  r->last_touched_frame = player->frame;
  r->duration = duration;
  return r;
}

An_Animation *an_interpolate_ex(An_Player *player, U64 hash_key, float duration, float *interpolate, Vector2 *interpolate_v) {
  An_Animation *r = an_get_persistent_data(player, hash_key);
  r->kind = An_Animation_Kind_Interpolate;
  r->last_touched_frame = player->frame;
  r->duration = duration;
  if (interpolate)   r->interpolate = interpolate;
  if (interpolate_v) r->interpolate_v = interpolate_v;
  return r;
}

An_Animation *an_translate_ex(An_Player *player, U64 hash_key, float duration, An_Shape *shape, Vector2 pos) {
  An_Animation * r = an_interpolate_ex(player, hash_key, duration, 0, &shape->pos);
  r->shape = shape;
  r->target_pos = pos;
  return r;
}

An_Animation *an_fade_in_ex(An_Player *player, U64 hash_key, float duration,
                            An_Shape *shape, float target_radius) {
  An_Animation *r = an_interpolate_ex(player, hash_key, duration, &shape->radius, 0);
  r->shape = shape;
  r->target_radius = target_radius;
  if (r->first_frame) {
    shape->hide = 1;
    shape->radius = 0.01f;
  }
  return r;
}

An_Animation *an_push_scheduler(An_Player *player, U64 hash_key, float stagger) {
  An_Animation *r = an_get_persistent_data(player, hash_key);
  r->stagger = stagger;
  // push
  An_Scheduler_Node *n = new(p->frame, An_Scheduler_Node, 1);
  n->node = r;
  n->next = player->scheduler;
  player->scheduler = n;
  return r;
}

void an_pop_scheduler(An_Player *player) {
  An_Animation *scheduler = player->scheduler->node;
  assert(scheduler);

  // apply to children ?
  {
    float end = 0.f;
    float running_duration = 0.f;
    for (An_Animation *child = scheduler->first; child; child = child->next) {
      child->beg = scheduler->beg + running_duration;
      end = fmax(end, child->beg + child->duration);
      if (scheduler->stagger <= 0.f) {
        running_duration += child->duration;
        running_duration -= child->stagger;
      }
      running_duration += scheduler->stagger;
    }
    scheduler->duration = end - scheduler->beg;
  }

  player->duration = fmax(player->duration, scheduler->beg + scheduler->duration);

  // pop
  player->scheduler = player->scheduler->next;
}

void *update(App_Update_Params params, void *pstate) {
  if (pstate == 0) { // Init
    p = (State *) arena_alloc(params.perm, MAX_STATE_CAP, _Alignof(State), 1);
    p->struct_size = size_of(*p);
    p->perm = params.perm;
    p->frame = params.frame;
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

  if (IsKeyPressed(KEY_SPACE)) {
    __builtin_memset(&p->player, 0, sizeof(p->player));
  }

  BeginDrawing();
  ClearBackground(BLACK);
  Camera2D camera = { .zoom = min(GetRenderWidth() / 10.f, GetRenderHeight() / 10.f), };
  camera.offset = (Vector2) { GetRenderWidth() / 2.f, GetRenderHeight() / 2.f };

  BeginMode2D(camera);
  float w = ( GetRenderWidth() / camera.zoom );
  float hw = w / 2;
  float h = ( GetRenderHeight() / camera.zoom );
  float hh = h / 2;

  An_Player *player = &p->player;
  an_beg(player, GetFrameTime());
  {
    /* an_slide("1st slide") */
    Rectangle corners = { -hw * 0.5f, -hh * 0.5f, hh*0.5f, hh*0.5f };
    An_Shape *circle = an_circle(((Vector2){corners.x + 0, 0.f}), 1.f);

    An_Animation *test = an_push_scheduler(player, (FUNLINE_HASH), 0.f);
      an_play(an_fade_in(circle, 2.f, 1.f));
      an_play(an_translate(circle, 2.f, ((Vector2){ 0, 0 })));
    an_pop_scheduler(player);

    an_push_child(player->scheduler->node, test);

    an_play(an_translate(circle, 2.1f, ((Vector2){ corners.x, 0 })));
  }
  an_end(&p->player, h, w);
  EndMode2D();

  EndDrawing();

  return p;
}

#endif
