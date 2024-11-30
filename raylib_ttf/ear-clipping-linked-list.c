#if IN_SHELL /* $ bash ear-clipping-linked-list.c
cc ear-clipping-linked-list.c -o ear-clipping-linked-list  -fsanitize=undefined -Wall -g3 -O0 -lpthread -lraylib
cc ear-clipping-linked-list.c -o app.so -DBUILD_RELOADABLE -fsanitize=undefined -Wall -g3 -O0 -shared -fPIC -lraylib
cc ear-clipping-linked-list.c -o ear-clipping-linked-list -DAMALGAMATION -Wall -Wextra -O3 -lpthread -lraylib -lm
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
#define assert(c)    while((!(c))) __builtin_unreachable()
#define new(a, t, n) ((t *) arena_alloc(a, size_of(t), (Size)_Alignof(t), (n)))

typedef struct { U8 *beg, *end; } Arena;

__attribute((malloc, alloc_size(2,4), alloc_align(3)))
static U8 *arena_alloc(Arena *a, Size objsize, Size align, Size count) {
  Size padding = -(USize)(a->beg) & (align - 1);
  Size total   = padding + objsize * count;
  if (total >= (a->end - a->beg)) {
    assert(0);
    TraceLog(LOG_FATAL, "Out of memory.");
  }
  U8 *p = a->beg + padding;
  __builtin_memset(p, 0, objsize * count);
  a->beg += total;
  return p;
}

////////////////////////////////////////////////////////////////////////////////
//- Executable

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

int main()
{
  enum {
    HEAP_CAP = 1ll << 30,
    FRAME_CAP = 1ll << 24,
  };
  void *heap = MemAlloc(HEAP_CAP);

  Arena arena = (Arena){heap, heap + HEAP_CAP};
  Arena frame = {0}; {
    frame.beg = new(&arena, U8, FRAME_CAP);
		frame.end = frame.beg + FRAME_CAP;
  }
  const Arena reset_arena = arena;
  const Arena reset_frame = frame;

	AppCode app_code = {0};
  void *app_state = 0;

  SetConfigFlags(FLAG_MSAA_4X_HINT);
  SetWindowState(FLAG_WINDOW_RESIZABLE);

	InitWindow(800,800, "hk: ear clipping");
	while (!WindowShouldClose()) {
		app_code = maybe_load_or_reload_app_code(app_code, IsKeyReleased(KEY_R));
    app_state = app_code.update(&arena, &frame, app_state);
		frame = reset_frame;
    if (IsKeyReleased(KEY_F5)) { arena = reset_arena; app_state = 0; }
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
#define MAX_NODES (256)

////////////////////////////////
//~ rjf: Linked List Building Macros

//- rjf: linked list macro helpers
#define CheckNil(nil,p) ((p) == 0 || (p) == nil)
#define SetNil(nil,p) ((p) = nil)

//- rjf: doubly-linked-lists
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

//- rjf: singly-linked, doubly-headed lists (queues)
#define SLLQueuePush_NZ(nil,f,l,n,next) (CheckNil(nil,f)?\
((f)=(l)=(n),SetNil(nil,(n)->next)):\
((l)->next=(n),(l)=(n),SetNil(nil,(n)->next)))
#define SLLQueuePushFront_NZ(nil,f,l,n,next) (CheckNil(nil,f)?\
((f)=(l)=(n),SetNil(nil,(n)->next)):\
((n)->next=(f),(f)=(n)))
#define SLLQueuePop_NZ(nil,f,l,next) ((f)==(l)?\
(SetNil(nil,f),SetNil(nil,l)):\
((f)=(f)->next))

//- rjf: singly-linked, singly-headed lists (stacks)
#define SLLStackPush_N(f,n,next) ((n)->next=(f), (f)=(n))
#define SLLStackPop_N(f,next) ((f) ? (f)=(f)->next : 0)

//- rjf: doubly-linked-list helpers
#define DLLInsert_NP(f,l,p,n,next,prev) DLLInsert_NPZ(0,f,l,p,n,next,prev)
#define DLLPushBack_NP(f,l,n,next,prev) DLLPushBack_NPZ(0,f,l,n,next,prev)
#define DLLPushFront_NP(f,l,n,next,prev) DLLPushFront_NPZ(0,f,l,n,next,prev)
#define DLLRemove_NP(f,l,n,next,prev) DLLRemove_NPZ(0,f,l,n,next,prev)
#define DLLInsert(f,l,p,n) DLLInsert_NPZ(0,f,l,p,n,next,prev)
#define DLLPushBack(f,l,n) DLLPushBack_NPZ(0,f,l,n,next,prev)
#define DLLPushFront(f,l,n) DLLPushFront_NPZ(0,f,l,n,next,prev)
#define DLLRemove(f,l,n) DLLRemove_NPZ(0,f,l,n,next,prev)

//- rjf: singly-linked, doubly-headed list helpers
#define SLLQueuePush_N(f,l,n,next) SLLQueuePush_NZ(0,f,l,n,next)
#define SLLQueuePushFront_N(f,l,n,next) SLLQueuePushFront_NZ(0,f,l,n,next)
#define SLLQueuePop_N(f,l,next) SLLQueuePop_NZ(0,f,l,next)
#define SLLQueuePush(f,l,n) SLLQueuePush_NZ(0,f,l,n,next)
#define SLLQueuePushFront(f,l,n) SLLQueuePushFront_NZ(0,f,l,n,next)
#define SLLQueuePop(f,l) SLLQueuePop_NZ(0,f,l,next)

//- rjf: singly-linked, singly-headed list helpers
#define SLLStackPush(f,n) SLLStackPush_N(f,n,next)
#define SLLStackPop(f) SLLStackPop_N(f,next)

#include <raymath.h>

static float exp_decay(float a, float b, float decay, float dt)
{
  return b + (a - b) * exp(-decay * dt);
}

static Vector2 exp_decay_v(Vector2 a, Vector2 b, float decay, float dt)
{
  Vector2 r = {};
  r.x = exp_decay(a.x, b.x, decay, dt);
  r.y = exp_decay(a.y, b.y, decay, dt);
  return r;
}

static float smooth_step(float x, float edge0, float edge1) {
  float t = (x - edge0) / (edge1 - edge0);
  t = __builtin_fmax(t, 0.f);
  t = __builtin_fmin(t, 1.f);
  return t * t * (3.0f - 2.0f * t);
}

static Camera2D camera_update(Camera2D camera, _Bool *mouse_input_consumed) {
  _Bool dummy;
  if (camera.zoom == 0.f) { camera.zoom = 1.0f; }
  if (mouse_input_consumed == 0) { mouse_input_consumed = &dummy; }

  Camera2D result = camera;

  if (IsKeyDown(KEY_LEFT_ALT) && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
    Vector2 delta = GetMouseDelta();
    delta = Vector2Scale(delta, -1.0f/camera.zoom);
    result.target = Vector2Add(camera.target, delta);
    *mouse_input_consumed = 1;
  }

   // Zoom based on mouse wheel
   float wheel = GetMouseWheelMove();
   if (wheel != 0) {
     // Offset and target
     Vector2 mouse_world_pos = GetScreenToWorld2D(GetMousePosition(), camera);
     result.offset = GetMousePosition();
     result.target = mouse_world_pos;

     // Zoom increment
     float scale_factor = 1.0f + (0.25f*fabsf(wheel));
     if (wheel < 0) scale_factor = 1.0f/scale_factor;
     result.zoom = Clamp(camera.zoom * scale_factor, 0.125f, 64.0f);

     *mouse_input_consumed = 1;
   }

  return result;
}

enum Flags {
  NodeFlags_CCW = 1 << 0, // counter clockwise means that the contour is a hole
  NodeFlags_EarTip = 1 << 1,
};
typedef struct Node Node;
struct Node {
  Node *next;
  Node *prev;

  U32 flags;
  Vector2 pos; // in contour's space
  struct {
    Vector2 pos;
  } animated;
};
typedef struct Contour Contour;
struct Contour {
  Contour *next;
  Contour *prev;

  Node *first;
  Node *last;
  Contour *bridge;

  U32 flags;
  float area;
};

typedef struct ContourList ContourList;
struct ContourList {
  Contour *first, *last;
  Node *free_list;
};

#define contour_3_points(contour) ((contour) && (contour)->first && (contour)->first != (contour)->last && (contour)->first->next != (contour)->last)

Contour *contour_make(Arena *arena, Vector2 center, Node **free_list, _Bool cw_winding) {
  Contour *result = new (arena, Contour, 1);
  for (Size i = 0; i < 3; i++) {
    Node *n = SLLStackPop(*free_list);
    if (n == 0) { n = new(arena, Node, 1); }
    float theta = M_PI + M_PI_2 * i;
    if (cw_winding) { theta *= -1; }
    float rad = 80.f;
    n->pos = (Vector2){ center.x + cosf(theta) * rad, center.y - sinf(theta) * rad };
    DLLPushBack(result->first, result->last, n);
  }
  return result;
}

Contour *contour_deep_copy(Arena *arena, Contour *contour) {
  Contour *result = 0;

  Size debug_node_count = 0;

  Contour *nc_prev = 0;
  for (Contour *c = contour; c; c = c->next) {
    Contour *nc = new(arena, Contour, 1);
    __builtin_memcpy(nc, c, sizeof(*nc));
    nc->first = nc->last = 0;
    nc->next = 0;
    if (result == 0) result = nc;
    if (nc_prev) nc_prev->next = nc;

    for (Node *n = c->first; n; n = n->next) {
      Node *nn = new(arena, Node, 1);
      __builtin_memcpy(nn, n, sizeof(*nn));
      nn->next = nn->prev = 0;
      DLLPushBack_NPZ(0, nc->first, nc->last, nn, next, prev);
      debug_node_count++;
    }
    nc_prev = nc;
  }

  return result;
}

typedef struct State {
  Size struct_size;
  Arena *perm;
  Arena *frame;

  Camera2D camera;
  Camera2D target_camera;
  ContourList contours;
} State;

static State *p = 0;
void *update(Arena *perm, Arena *frame, void *pstate) {
  if (pstate == 0) { // Init
    p = (State *) arena_alloc(perm, STATE_STRUCT_CAP, _Alignof(State), 1);
    p->struct_size = size_of(*p);
    p->perm = perm;
    p->frame = frame;
  }
  if (perm == 0 && frame == 0) { // Pre-reload
    TraceLog(LOG_INFO, "Reload.");
    return p;
  }
  if (p == 0) {    // Post-reload
    State *prev_p = (State *)pstate;
    if (prev_p->struct_size != size_of(*p)) {
      TraceLog(LOG_WARNING, "State schema resized: %ld -> %ld bytes.", prev_p->struct_size, size_of(*p));
    }
    p = prev_p;
    p->struct_size = size_of(*p);
  }

  float dt = GetFrameTime();

  _Bool mouse_input_consumed = 0;
  p->target_camera = camera_update(p->target_camera, &mouse_input_consumed);
  p->camera.zoom   = exp_decay  (p->camera.zoom,   p->target_camera.zoom, 12.f, dt);
  p->camera.offset = exp_decay_v(p->camera.offset, p->target_camera.offset, 12.f, dt);
  p->camera.target = exp_decay_v(p->camera.target, p->target_camera.target, 12.f, dt);

  BeginDrawing();
  BeginMode2D(p->camera);
  ClearBackground(BLACK);

  static Node    *hovered_node         = 0;
  static Contour *hovered_node_contour = 0;

  Vector2 mouse_world_pos = GetScreenToWorld2D(GetMousePosition(), p->camera);
  float   node_radius     = 14.f;

  if (!mouse_input_consumed && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
    if (hovered_node) {
      hovered_node->pos = mouse_world_pos;
    }
  } else {
    hovered_node = 0;
  }

  for (Contour *c = p->contours.first; !hovered_node && c; c = c->next) {
    for (Node *n = c->first; !hovered_node && n; n = n->next) {
      if (Vector2Distance(n->pos, mouse_world_pos) < node_radius) {
        hovered_node = n;
        hovered_node_contour = c;
      }
    }
  }
  if (!mouse_input_consumed && IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
    if (hovered_node) {
      DLLRemove(hovered_node_contour->first, hovered_node_contour->last,
                hovered_node);
      SLLStackPush(p->contours.free_list, hovered_node);
      hovered_node = 0;

      if (!contour_3_points(hovered_node_contour)) {
        Node *next = 0;
        for (Node *remaining = hovered_node_contour->first; remaining;
             remaining = next) {
          next = remaining->next;
          __builtin_memset(remaining, 0, sizeof *remaining);
          SLLStackPush(p->contours.free_list, remaining);
        }
        DLLRemove(p->contours.first, p->contours.last, hovered_node_contour);
        hovered_node_contour = 0;
      }
    } else if (p->contours.first) {
      // create contour hole
      Contour *parent = p->contours.first; // TODO: contour that has point inside
      Contour *child = contour_make(p->perm, mouse_world_pos, &p->contours.free_list, 1);
      SLLStackPush(parent->bridge, child);
    }
  }

  Contour *insert_contour = 0;
  Node *insertion_point = 0;
  {
    for (Contour *c = p->contours.first; c; c = c->next) {
      insert_contour = c;
      Node *prev = c->last;
      for (Node *n = c->first; n; n = n->next) {
        if (CheckCollisionPointLine(mouse_world_pos, prev->pos, n->pos,
                                    node_radius)) {
          { // Visualize possible node insertion
            Vector2 dir = Vector2Normalize(Vector2Subtract(n->pos, prev->pos));
            float projection_along_dir = Vector2DotProduct(dir, Vector2Subtract(mouse_world_pos, prev->pos));
            Vector2 point_on_line = Vector2Add(prev->pos, Vector2Scale(dir, projection_along_dir));
            DrawCircleV(point_on_line, node_radius / p->camera.zoom, ColorAlpha(WHITE, 0.3));
          }
          insertion_point = prev;
          goto point_found;
        }
        prev = n;
      }
      if (!contour_3_points(c)) {
        insertion_point = c->last;
      }
    }
  point_found:;
  }

  if (!mouse_input_consumed && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
    if (hovered_node) {
    } else {

      if (insertion_point == 0) {
        Contour *contour = contour_make(p->perm, mouse_world_pos, &p->contours.free_list, 0);
        DLLPushBack(p->contours.first, p->contours.last, contour);
      } else {
        Node *new_node = SLLStackPop(p->contours.free_list);
        if (!new_node) { new_node = new (p->perm, Node, 1); }
        new_node->pos = mouse_world_pos;
        new_node->animated.pos = insertion_point ? insertion_point->pos : new_node->pos;
        DLLInsert(insert_contour->first, insert_contour->last, insertion_point, new_node);
      }
    }
    hovered_node = 0;
  }

  // Update flags
  for (Contour *c = p->contours.first; c; c = c->next) {
    for (Node *n = c->first; n; n = n->next) {
      n->flags = 0; // reset flag
      n->animated.pos =
        exp_decay_v(n->animated.pos, n->pos, 32.f, dt);

      float contour_area = 0.f;
      for (Node *l = c->last, *r = c->first; l && r; l = r, r = r->next) {
        float height = (l->pos.y + r->pos.y) * 0.5f;
        float width = r->pos.x - l->pos.x;
        contour_area += height * width;
      }
      c->area = contour_area;
    }
  }

  { // ear cutting algorithm
    Arena temp = *p->frame;

    Contour *working_list = contour_deep_copy(&temp, p->contours.first);

    for (Contour *c = working_list; contour_3_points(c); c = c->next) {

      // Construct bridges for contour holes
      for (Contour *hole = c->bridge; contour_3_points(hole);
           hole = hole->next) {

        for (Node *n = hole->first; c; n = n->next) {

        }

      }

      // connect contour
      c->first->prev = c->last;
      c->last->next = c->first;

      _Bool is_final_tri = 0;
      Size max_iterations = MAX_NODES * 4;
      for (Node *n = c->first; !is_final_tri && n && --max_iterations; n = n->next) {
        Node *prev = n->prev;
        Node *next = n->next;

        is_final_tri = (next == prev) || (next->next == prev);

        _Bool is_ccw = 0;
        {
          Vector2 v = Vector2Subtract(n->pos, prev->pos);
          Vector2 w = Vector2Subtract(next->pos, prev->pos);
          float det = (v.x * w.y) - (w.x * v.y);
          is_ccw = det < 0.f;
        }
        n->flags |= !!is_ccw * NodeFlags_CCW;

        _Bool tri_contain_node = 0;
        Size max_search_tries = MAX_NODES;
        for (Node *o = next->next; !is_final_tri && --max_search_tries > 0 && o != prev; o = o->next) {
          if (CheckCollisionPointTriangle(o->pos, prev->pos, n->pos,
                                          next->pos)) {
            tri_contain_node = 1;
            break;
          }
        }
        if (max_search_tries == 0) {
          TraceLog(LOG_WARNING, "Searching for other nodes resulted in an infinite loop. Something is seriously wrong.");
          break;
        }

        if ((is_ccw && !tri_contain_node) || is_final_tri) {
          Color color = ColorFromHSV((U32)n & (256 - 1), 1.f, 1.f);
          DrawTriangle(prev->pos, n->pos, next->pos, ColorAlpha(color, 0.5f));
          DLLRemove(c->first, c->last, n);
        }
      }
      if (max_iterations == 0) {
        TraceLog(LOG_WARNING, "Reached maximum node tries. Seems that some nodes are not getting removed.");
        break;
      }
    }
  }

  // Render
  for (Contour *c = p->contours.first; c; c = c->next) {
    // Draw winding arrow
    if (contour_3_points(c)) {
      Vector2 arrow_center = c->first->animated.pos;
      float arrow_radius = node_radius + node_radius * 0.25f;
      _Bool arrow_ccw = c->area > 0;
      float arror_width = 6.f;
      { // draw circle arrow
        float off = arrow_radius;
        float theta[] = {3.f * M_PI_4, 2.f * M_PI_4, 1 * M_PI_4};
        Vector2 points[] = {
          Vector2Add(arrow_center, (Vector2){off * cosf(theta[0]), -off * sinf(theta[0])}),
          Vector2Add(arrow_center, (Vector2){off * cosf(theta[1]), -off * sinf(theta[1])}),
          Vector2Add(arrow_center, (Vector2){off * cosf(theta[2]), -off * sinf(theta[2])}),
        };
        DrawSplineBezierQuadratic(points, count_of(points), arror_width * 0.4, RED);

        Vector2 tip_point = arrow_ccw ? points[0] : points[2];
        float dx = 0.0001f;
        Vector2 dx_end_point = GetSplinePointBezierQuad(points[0], points[1], points[2], dx + (arrow_ccw ? 0.0f : (1.f - dx * 2.f)));
        Vector2 arrow_dir = Vector2Subtract(tip_point, dx_end_point);
                arrow_dir = Vector2Normalize(arrow_dir);
        Vector2 arrow_normal = Vector2Scale((Vector2){ -arrow_dir.y, arrow_dir.x }, arror_width);

        Vector2 arrow_tip = Vector2Add(tip_point, Vector2Scale(arrow_dir, arror_width * 1.8f));
        Vector2 arrow_tip_base_l = Vector2Subtract(tip_point, arrow_normal);
        Vector2 arrow_tip_base_r = Vector2Add(tip_point, arrow_normal);
        DrawTriangle(arrow_tip, arrow_tip_base_l, arrow_tip_base_r, RED);
      }
    }

    // Draw node circles
    for (Node *n = c->first, *prev = c->last; n; prev = n, n = n->next) { DrawLineV(prev->pos, n->pos, BLUE); }
    for (Node *n = c->first; n; n = n->next) {
      float saturation = (n == hovered_node) ? 1.0f : 0.2f;
      float value = 1.0f;
      float alpha = smooth_step(Vector2Distance(mouse_world_pos, n->pos), 100.0f, 0.f);
      Color color =
        ColorFromHSV((float)((U32)n & (256 - 1)), saturation, value);
      color = ColorAlpha(color, alpha);
      DrawCircleV(n->animated.pos, node_radius, color);
    }
  }

  EndMode2D();
  EndDrawing();

  return p;
}

#endif
