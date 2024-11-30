#if 0
# bash main.c
cc main.c -fsanitize=undefined -g3 -lpthread -lraylib
cc app.c -o app.so -fsanitize=undefined -g3 -shared -lpthread -lraylib
exit
#endif


/*
  #+title: Minimal, hot-reloadable, creative coding template
  #+author: anton@hakanssn.com
  #+licence: This is free and unencumbered software released into the public domain.
  */

#include <stdlib.h>
#include <raylib.h>

#include "app.h"

typedef struct {
  void *handle;
  void *(*update)(Arena *, Arena *, void *);
} AppCode;

#include <dlfcn.h>
AppCode maybe_load_or_reload_app_code(AppCode cur, _Bool should_reload)
{
  AppCode result = cur;
  _Bool should_init = (cur.handle == 0);

  if (should_reload) {
    assert(cur.handle);
    void *dummy = cur.handle;
    cur.update(0, 0, dummy); // nofify pre-reload
    dlclose(cur.handle);
  }

  if (should_init || should_reload) {
    result = (AppCode){0};
    result.handle = dlopen("./app.so", RTLD_LAZY);
    assert(result.handle);
    result.update = dlsym(result.handle, "update");
    assert(result.update);
  }

  return result;
}

static void run(Arena arena)
{
  Size frame_cap = 1ll << 21;
  Arena frame = {0}; {
    frame.beg = new(&arena, U8, frame_cap);
    frame.end = frame.beg + frame_cap;
  }

  AppCode app_code = {0};
  void *app_state = 0;

  InitWindow(800, 600, "dynamic reload");
  while (!WindowShouldClose()) {
    app_code = maybe_load_or_reload_app_code(app_code, IsKeyReleased(KEY_R));
    app_state = app_code.update(&arena, &frame, app_state);
    frame.beg = frame.end - frame_cap; // rewind frame arena
  }
  CloseWindow();
}

int main(int argc, char **argv)
{
  Size HEAP_CAP = 1ll << 30;
  void *heap = malloc(HEAP_CAP);
  Arena arena = (Arena){heap, heap + HEAP_CAP};
  run(arena);
  free(heap);
  return 0;
}

/* Local Variables: */
/* compile-command: "bash main.c" */
/* End: */
