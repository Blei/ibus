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

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <gdk/gdk.h>

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
    IBusInputContext *context;

    char *preedit_string;
    IBusAttrList *preedit_attrs;
    uint32_t preedit_cursor;
    bool preedit_visible;
    bool preedit_started;

    bool focused;
};

static void
reset_ibus_ime(struct ibus_ime *ime)
{
    if (ime->preedit_string) {
        free(ime->preedit_string);
        ime->preedit_string = NULL;
    }
    if (ime->preedit_attrs) {
        g_object_unref (ime->preedit_attrs);
        ime->preedit_attrs = NULL;
    }
    ime->preedit_cursor = 0;
    ime->preedit_visible = false;
    ime->preedit_started = false;
    ime->focused = false;
}

static void
input_method_reset(void *data,
                   struct input_method *input_method)
{
    struct ibus_ime *ime = data;

    // HACK: this is here because wayland/weston doesn't do ime focus yet
    if (ime->focused)
        ibus_input_context_focus_out(ime->context);

    ibus_input_context_reset(ime->context);
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
translate_key(GdkEventKey *event,
              xkb_keysym_t sym,
              struct ibus_ime *ime,
              enum wl_keyboard_key_state state,
              uint32_t keycode,
              uint32_t time)
{
    event->time = time;
    event->window = NULL;
    event->send_event = 0; // FIXME what to do?
    event->type = state == WL_KEYBOARD_KEY_STATE_RELEASED ? GDK_KEY_RELEASE : GDK_KEY_PRESS;
    event->group = 0; // FIXME this should be possible to find out
    event->hardware_keycode = keycode;

    event->state = 0;
    if (ime->modifiers & MOD_CONTROL_MASK)
        event->state |= GDK_CONTROL_MASK;
    if (ime->modifiers & MOD_SHIFT_MASK)
        event->state |= GDK_SHIFT_MASK;
    if (ime->modifiers & MOD_ALT_MASK)
        event->state |= GDK_MOD1_MASK;
    if (state == WL_KEYBOARD_KEY_STATE_RELEASED)
        event->state |= IBUS_RELEASE_MASK;

    // Looks wrong, but is equivalent to what the XIM backend does.
    event->is_modifier = 0;
    event->string = NULL;
    event->length = 0;

    // HACK: The gdk keysyms are auto generated from the xkb ones, this should
    // be equivalent. Did some spot checks, everything looks correct.
    event->keyval = sym;
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
    fprintf(stderr, "received key %d\n", key);

    // XXX: copied from window.c
    uint32_t code, num_syms;
    enum wl_keyboard_key_state state = state_w;
    const xkb_keysym_t *syms;
    xkb_keysym_t sym;
    xkb_mod_mask_t mask;

    // HACK: this is here because wayland/weston doesn't do ime focus yet
    if (!ime->focused) {
        //ibus_input_context_focus_in(ime->context);
        ime->focused = true;
    }

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
    // end of copied code

    GdkEventKey event;
    translate_key(&event, sym, ime, state, key, time);

    gboolean retval = ibus_input_context_process_key_event (
        ime->context,
        event.keyval,
        event.hardware_keycode - 8,
        event.state);

    fprintf(stderr, "input has %sbeen processed\n", retval ? "" : "not ");

    if (!retval) {
        // The key event could not be processed correctly.
        // TODO forward the key event to the application
    }
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
_context_commit_text_cb (IBusInputContext *context,
                         IBusText         *text,
                         struct ibus_ime  *ime)
{
    g_assert (IBUS_IS_INPUT_CONTEXT (context));
    g_assert (IBUS_IS_TEXT (text));
    g_assert (ime != NULL);

    fprintf(stderr, "commit with text '%s'\n", text->text);

    input_method_commit_string(ime->input_method, text->text, -1);
}

static void
_context_forward_key_event_cb (IBusInputContext *context,
                               guint             keyval,
                               guint             keycode,
                               guint             state,
                               struct ibus_ime  *ime)
{
    g_assert (ime);

    fprintf(stderr, "forward key with keyval '%d'\n", keyval);

    // TODO implement this as soon as wayland/weston supports it
}

static void
_update_preedit(struct ibus_ime *ime)
{
    if (ime->preedit_visible) {
        if (!ime->preedit_started) {
            ime->preedit_started = true;
        }
        input_method_preedit_string(ime->input_method, ime->preedit_string,
                                    ime->preedit_cursor);
    } else if (ime->preedit_started) {
        input_method_preedit_string(ime->input_method, "", 0);
        ime->preedit_started = false;
    }
}

static void
_context_update_preedit_text_cb (IBusInputContext *context,
                                 IBusText         *text,
                                 gint              cursor_pos,
                                 gboolean          visible,
                                 struct ibus_ime  *ime)
{
    g_assert (IBUS_IS_INPUT_CONTEXT (context));
    g_assert (IBUS_IS_TEXT (text));
    g_assert (ime);

    fprintf(stderr, "update preedit with string '%s'\n", text->text);

    if (ime->preedit_string) {
        free(ime->preedit_string);
    }
    ime->preedit_string = strdup(text->text);

    if (ime->preedit_attrs) {
        g_object_unref (ime->preedit_attrs);
    }

    g_object_ref(text->attrs);
    ime->preedit_attrs = text->attrs;

    ime->preedit_cursor = cursor_pos;
    ime->preedit_visible = visible;

    _update_preedit (ime);
}

static void
_context_show_preedit_text_cb (IBusInputContext *context,
                               struct ibus_ime  *ime)
{
    g_assert (IBUS_IS_INPUT_CONTEXT (context));
    g_assert (ime);

    fprintf(stderr, "show preedit\n");

    ime->preedit_visible = true;
    _update_preedit (ime);
}

static void
_context_hide_preedit_text_cb (IBusInputContext *context,
                               struct ibus_ime  *ime)
{
    g_assert (IBUS_IS_INPUT_CONTEXT (context));
    g_assert (ime);

    fprintf(stderr, "hide preedit\n");

    ime->preedit_visible = false;
    _update_preedit (ime);
}

static void
_context_enabled_cb (IBusInputContext *context,
                     struct ibus_ime  *ime)
{
    g_assert (IBUS_IS_INPUT_CONTEXT (context));
    g_assert (ime);

    fprintf(stderr, "enabled\n");

    // Is there anything that needs to be done here?
}

static void
_context_disabled_cb (IBusInputContext *context,
                      struct ibus_ime  *ime)
{
    g_assert (IBUS_IS_INPUT_CONTEXT (context));
    g_assert (ime);

    fprintf(stderr, "disabled\n");

    reset_ibus_ime(ime);
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
_init_ibus_bus (struct ibus_ime *ime)
{
    if (ime->bus != NULL)
        return;

    ibus_init ();

    ime->bus = ibus_bus_new ();

    g_signal_connect (ime->bus, "disconnected",
                      G_CALLBACK (_bus_disconnected_cb), NULL);
}

static void
_init_ibus_context (struct ibus_ime *ime)
{
    ime->context = ibus_bus_create_input_context (ime->bus, "wayland");
    assert(ime->context && "ibus context couldn't be created");

    g_signal_connect (ime->context, "commit-text",
                        G_CALLBACK (_context_commit_text_cb), ime);
    g_signal_connect (ime->context, "forward-key-event",
                        G_CALLBACK (_context_forward_key_event_cb), ime);

    g_signal_connect (ime->context, "update-preedit-text",
                        G_CALLBACK (_context_update_preedit_text_cb), ime);
    g_signal_connect (ime->context, "show-preedit-text",
                        G_CALLBACK (_context_show_preedit_text_cb), ime);
    g_signal_connect (ime->context, "hide-preedit-text",
                        G_CALLBACK (_context_hide_preedit_text_cb), ime);
    g_signal_connect (ime->context, "enabled",
                        G_CALLBACK (_context_enabled_cb), ime);
    g_signal_connect (ime->context, "disabled",
                        G_CALLBACK (_context_disabled_cb), ime);

    // TODO add IBUS_CAP_SURROUNDING_TEXT once wayland/weston supports that
    ibus_input_context_set_capabilities (ime->context,
                                         IBUS_CAP_FOCUS | IBUS_CAP_PREEDIT_TEXT);
}

static void
init_ibus (struct ibus_ime *ime)
{
    _init_ibus_bus(ime);
    _init_ibus_context(ime);
}

int
main(int argc, char *argv[])
{
    struct ibus_ime ime;
    memset(&ime, 0, sizeof ime);

    g_type_init();

    init_ibus(&ime);

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

    // XXX integrate the weston & glib main loops!
    // maybe add the epolls called in display_run as a GSoure?
    display_run(ime.display);

    g_object_unref (ime.bus);
    xkb_state_unref(ime.xkb.state);
    xkb_map_unref(ime.xkb.keymap);

    return 0;
}
