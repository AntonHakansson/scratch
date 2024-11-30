#include "app.h"
#include "raylib.h"

#define STATE_STRUCT_CAP (1 << 12)
typedef struct {
  Size struct_size;

  Arena *frame;
  Arena *perm;

} State;
State *p = 0;

void *update(Arena *perm, Arena *frame, void *pstate)
{
  if (pstate == 0) { // Init
    p = (State *) arena_alloc(perm, STATE_STRUCT_CAP, _Alignof(*p), 1);
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
      TraceLog(LOG_INFO, "Resized State schema %ld -> %ld", prev_p->struct_size, size_of(*p));
    }
    p = prev_p;
    p->struct_size = size_of(*p);
  }

  // Update
  BeginDrawing();
  ClearBackground(BLACK);
    DrawRectangle(0, 0, 80, 80, RED);
    DrawRectangle(100, 0, 80, 80, BLUE);
    DrawRectangle(100, 100, 80, 80, YELLOW);
    DrawRectangle(0, 100, 80, 80, GREEN);
  EndDrawing();

  return p;
}
