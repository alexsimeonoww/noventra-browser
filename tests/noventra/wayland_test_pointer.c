// Isolated compositor test input. Never connect this helper to the host desktop.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>
#include "virtual-pointer.h"

static struct zwlr_virtual_pointer_manager_v1 *manager;
static void global(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version) {
  if (!strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name))
    manager = wl_registry_bind(registry, name,
                               &zwlr_virtual_pointer_manager_v1_interface, 1);
}
static void removed(void *data, struct wl_registry *registry, uint32_t name) {}
static const struct wl_registry_listener listener = {global, removed};
static uint32_t now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}
int main(void) {
  const char *private = getenv("NOVENTRA_PRIVATE_WAYLAND");
  const char *runtime = getenv("XDG_RUNTIME_DIR");
  const char *socket = getenv("WAYLAND_DISPLAY");
  if (!private || strcmp(private,"1") || !runtime || strncmp(runtime,"/tmp/nv-",8)
      || !socket || strchr(socket,'/')) return 2;
  struct wl_display *display = wl_display_connect(NULL);
  assert(display);
  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &listener, NULL);
  assert(wl_display_roundtrip(display) >= 0 && manager);
  struct zwlr_virtual_pointer_v1 *pointer =
      zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, NULL);
  assert(wl_display_roundtrip(display) >= 0);
  char line[256];
  while (fgets(line, sizeof(line), stdin)) {
    unsigned x, y, width, height, down, code = 272;
    if (sscanf(line,"motion %u %u %u %u",&x,&y,&width,&height) == 4)
      zwlr_virtual_pointer_v1_motion_absolute(pointer,now(),x,y,width,height);
    else if (sscanf(line,"button %u %u",&down,&code) >= 1 &&
             (code == 272 || code == 273 || code == 274))
      zwlr_virtual_pointer_v1_button(pointer,now(),code,down ? 1 : 0);
    else return 3;
    zwlr_virtual_pointer_v1_frame(pointer);
    if (wl_display_roundtrip(display) < 0) return 4;
    puts("ok"); fflush(stdout);
  }
  zwlr_virtual_pointer_v1_destroy(pointer);
  zwlr_virtual_pointer_manager_v1_destroy(manager);
  wl_registry_destroy(registry);
  wl_display_flush(display);
  wl_display_disconnect(display);
}
