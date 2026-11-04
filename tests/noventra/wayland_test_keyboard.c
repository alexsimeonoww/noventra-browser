// Native key codes with a standard US map, on an owned compositor only.
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "virtual-keyboard.h"

static struct zwp_virtual_keyboard_manager_v1 *manager;
static struct wl_seat *seat;
static void global(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version) {
  if (!strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name))
    manager = wl_registry_bind(registry, name,
        &zwp_virtual_keyboard_manager_v1_interface, 1);
  else if (!strcmp(interface, wl_seat_interface.name))
    seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
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
  if (!private || strcmp(private, "1") || !runtime || strncmp(runtime, "/tmp/nv-", 8)
      || !socket || strchr(socket, '/')) return 2;
  struct wl_display *display = wl_display_connect(NULL);
  assert(display);
  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &listener, NULL);
  assert(wl_display_roundtrip(display) >= 0 && manager && seat);
  struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  struct xkb_rule_names names = {.rules="evdev", .model="pc105", .layout="us"};
  struct xkb_keymap *map = xkb_keymap_new_from_names(context, &names,
                                                    XKB_KEYMAP_COMPILE_NO_FLAGS);
  assert(map);
  struct xkb_state *state = xkb_state_new(map);
  char *text = xkb_keymap_get_as_string(map, XKB_KEYMAP_FORMAT_TEXT_V1);
  size_t size = strlen(text) + 1;
  int fd = memfd_create("noventra-private-keymap", MFD_CLOEXEC);
  assert(fd >= 0 && write(fd, text, size) == (ssize_t)size);
  struct zwp_virtual_keyboard_v1 *keyboard =
      zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(manager, seat);
  zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
  assert(wl_display_roundtrip(display) >= 0);
  close(fd); free(text);
  usleep(200000);
  puts("ready"); fflush(stdout);
  char line[128];
  while (fgets(line, sizeof(line), stdin)) {
    unsigned key, down;
    if (sscanf(line, "key %u %u", &key, &down) != 2 || key > 255 || down > 1)
      return 3;
    zwp_virtual_keyboard_v1_key(keyboard, now(), key, down);
    xkb_state_update_key(state, key + 8, down ? XKB_KEY_DOWN : XKB_KEY_UP);
    zwp_virtual_keyboard_v1_modifiers(keyboard,
        xkb_state_serialize_mods(state, XKB_STATE_MODS_DEPRESSED),
        xkb_state_serialize_mods(state, XKB_STATE_MODS_LATCHED),
        xkb_state_serialize_mods(state, XKB_STATE_MODS_LOCKED),
        xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_EFFECTIVE));
    if (wl_display_roundtrip(display) < 0) return 4;
    puts("ok"); fflush(stdout);
  }
  zwp_virtual_keyboard_v1_destroy(keyboard);
  wl_registry_destroy(registry);
  wl_display_flush(display); wl_display_disconnect(display);
  xkb_state_unref(state); xkb_keymap_unref(map); xkb_context_unref(context);
}
