/* -*- mode: C; c-basic-offset: 4; indent-tabs-mode: nil; -*- */
/* vim:set et sts=4 sw=4: */
/* ibus
 * Copyright (C) 2012 Philipp Brüschweiler <blei42@gmail.com>
 *
 * This tool is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <ibus.h>

#include <text-client-protocol.h>
#include <window.h>

struct ibus_ime {
    struct display *display;
    struct input_method *input_method;

    uint32_t modifiers;
    struct {
        struct xkb_context *context;
        struct xkb_keymap *keymap;
        struct xkb_state *state;
        xkb_mod_mask_t control_mask;
        xkb_mod_mask_t alt_mask;
        xkb_mod_mask_t shift_mask;
    } xkb;

    IBusBus *bus;
};

static void
reset_ibus_ime(struct ibus_ime *ime)
{
    // TODO
}

static void
ime_handle_key(struct ibus_ime *ime, uint32_t key, uint32_t sym,
	       enum wl_keyboard_key_state state)
{
    // TODO
}

static void
input_method_reset(void *data,
                   struct input_method *input_method)
{
    struct ibus_ime *ime = data;
    reset_ibus_ime(ime);
}

static void
input_method_keymap(void *data,
                    struct input_method *input_method,
                    uint32_t format,
                    int32_t fd,
                    uint32_t size)
{
    struct ibus_ime *ime = data;
    fprintf(stderr, "received keymap\n");

    // XXX: this is copy-pasted from window.c
    char *map_str;

    if (!data) {
        close(fd);
        return;
    }

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    map_str = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (map_str == MAP_FAILED) {
        close(fd);
        return;
    }

    ime->xkb.keymap = xkb_map_new_from_string(ime->xkb.context,
                                              map_str,
                                              XKB_KEYMAP_FORMAT_TEXT_V1,
                                              0);
    munmap(map_str, size);
    close(fd);

    if (!ime->xkb.keymap) {
        fprintf(stderr, "failed to compile keymap\n");
        return;
    }

    ime->xkb.state = xkb_state_new(ime->xkb.keymap);
    if (!ime->xkb.state) {
        fprintf(stderr, "failed to create XKB state\n");
        xkb_map_unref(ime->xkb.keymap);
        ime->xkb.keymap = NULL;
        return;
    }

    ime->xkb.control_mask =
        1 << xkb_map_mod_get_index(ime->xkb.keymap, "Control");
    ime->xkb.alt_mask =
        1 << xkb_map_mod_get_index(ime->xkb.keymap, "Mod1");
    ime->xkb.shift_mask =
        1 << xkb_map_mod_get_index(ime->xkb.keymap, "Shift");
}

static void
input_method_key(void *data,
                 struct input_method *input_method,
                 uint32_t serial,
                 uint32_t time,
                 uint32_t key,
                 uint32_t state_w)
{
    struct ibus_ime *ime = data;
    //fprintf(stderr, "received key %d\n", key);

    // XXX: copied from window.c
    uint32_t code, num_syms;
    enum wl_keyboard_key_state state = state_w;
    const xkb_keysym_t *syms;
    xkb_keysym_t sym;
    xkb_mod_mask_t mask;

    code = key + 8;
    if (!ime->xkb.state)
        return;

    num_syms = xkb_key_get_syms(ime->xkb.state, code, &syms);

    mask = xkb_state_serialize_mods(ime->xkb.state,
                                    XKB_STATE_DEPRESSED |
                                    XKB_STATE_LATCHED);
    ime->modifiers = 0;
    if (mask & ime->xkb.control_mask)
        ime->modifiers |= MOD_CONTROL_MASK;
    if (mask & ime->xkb.alt_mask)
        ime->modifiers |= MOD_ALT_MASK;
    if (mask & ime->xkb.shift_mask)
        ime->modifiers |= MOD_SHIFT_MASK;

    sym = XKB_KEY_NoSymbol;
    if (num_syms == 1)
        sym = syms[0];

    ime_handle_key(ime, key, sym, state);
}

static void
input_method_modifiers(void *data,
                       struct input_method *input_method,
                       uint32_t serial,
                       uint32_t mods_depressed,
                       uint32_t mods_latched,
                       uint32_t mods_locked,
                       uint32_t group)
{
    struct ibus_ime *ime = data;
    //fprintf(stderr, "received modifiers %d\n", mods_depressed);

    // XXX: copied from window.c
    xkb_state_update_mask(ime->xkb.state, mods_depressed, mods_latched,
                          mods_locked, 0, 0, group);
}

static const struct input_method_listener input_method_listener = {
    input_method_reset,
    input_method_keymap,
    input_method_key,
    input_method_modifiers
};

static void
global_handler(struct wl_display *display, uint32_t id,
               const char *interface, uint32_t version, void *data)
{
    struct ibus_ime *ime = data;

    if (!strcmp(interface, "input_method")) {
        ime->input_method = wl_display_bind(display, id, &input_method_interface);
        input_method_add_listener(ime->input_method,
                                  &input_method_listener,
                                  ime);
        input_method_request_keyboard_input(ime->input_method, 1);
    }
}

static void
_bus_disconnected_cb (IBusBus  *bus,
                      gpointer  user_data)
{
    g_warning ("Connection closed by ibus-daemon");
    g_object_unref (bus);
    exit(EXIT_SUCCESS);
}

static void
_init_ibus (IBusBus **bus)
{
    if (*bus != NULL)
        return;

    ibus_init ();

    *bus = ibus_bus_new ();

    g_signal_connect (*bus, "disconnected",
                      G_CALLBACK (_bus_disconnected_cb), NULL);
}

int
main(int argc, char *argv[])
{
    struct ibus_ime ime;
    memset(&ime, 0, sizeof ime);

    g_type_init();

    _init_ibus (&ime.bus);

    if (!ibus_bus_is_connected (ime.bus)) {
        g_warning ("Can not connect to ibus daemon");
        exit (EXIT_FAILURE);
    }

    reset_ibus_ime(&ime);

    ime.xkb.context = xkb_context_new(0);
    if (ime.xkb.context == NULL) {
        fprintf(stderr, "Failed to create XKB context\n");
        return 1;
    }

    ime.display = display_create(argc, argv);
    if (ime.display == NULL) {
        fprintf(stderr, "failed to create display: %m\n");
        return 1;
    }

    wl_display_add_global_listener(display_get_display(ime.display),
                                   global_handler, &ime);

    display_set_user_data(ime.display, &ime);

    display_run(ime.display);

    g_object_unref (ime.bus);
    xkb_state_unref(ime.xkb.state);
    xkb_map_unref(ime.xkb.keymap);

    return 0;
}
