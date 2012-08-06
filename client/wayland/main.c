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
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/time.h>

#include <ibus.h>

#include <text-client-protocol.h>
#include <window.h>

struct ibus_ime_context {
    struct ibus_ime *ime;
    IBusInputContext *ibus_context;
    struct input_method_context *ime_context;

    char *preedit_string;
    IBusAttrList *preedit_attrs;
    uint32_t preedit_cursor;
    bool preedit_visible;
    bool preedit_started;

    bool focused;
};

struct ibus_ime {
    struct display *display;
    struct input_method *input_method;
    struct wl_keyboard *input_method_keyboard;

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
    struct ibus_ime_context *active_context;
};

struct ibus_ime_source {
    GSource source;
    GPollFD poll_fd;

    struct ibus_ime *ime;
};

static void
reset_ibus_ime_context(struct ibus_ime_context *context)
{
    if (context->preedit_string) {
        free(context->preedit_string);
        context->preedit_string = NULL;
    }
    if (context->preedit_attrs) {
        g_object_unref (context->preedit_attrs);
        context->preedit_attrs = NULL;
    }
    context->preedit_cursor = 0;
    context->preedit_visible = false;
    context->preedit_started = false;
    context->focused = false;
    ibus_input_context_reset(context->ibus_context);
}

static void
_context_commit_text_cb (IBusInputContext        *ibus_context,
                         IBusText                *text,
                         struct ibus_ime_context *context)
{
    g_assert (IBUS_IS_INPUT_CONTEXT (ibus_context));
    g_assert (IBUS_IS_TEXT (text));
    g_assert (context);

    input_method_context_commit_string(context->ime_context, text->text, -1);
}

static void
_context_forward_key_event_cb (IBusInputContext        *ibus_context,
                               guint                    keyval,
                               guint                    keycode,
                               guint                    state,
                               struct ibus_ime_context *context)
{
    g_assert (context);

    fprintf(stderr, "forward key event\n");

    uint32_t key = keycode + 8;
    enum wl_keyboard_key_state state_w = state & IBUS_RELEASE_MASK ?
        WL_KEYBOARD_KEY_STATE_RELEASED : WL_KEYBOARD_KEY_STATE_PRESSED;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint32_t time = tv.tv_sec * 1000 + tv.tv_usec / 1000;

    input_method_context_forward_key(context->ime_context, time, key, state_w);
}

static void
_send_preedit(struct ibus_ime_context *context)
{
    int i;
    IBusAttribute *ibus_attr;
    char *str;

    assert(context && "no active context");

    str = context->preedit_string;
    input_method_context_preedit_string(context->ime_context, str, context->preedit_cursor);

    for (i = 0; (ibus_attr = ibus_attr_list_get(context->preedit_attrs, i)) != NULL ; ++i) {
        uint32_t type, value;

        uint32_t start = g_utf8_offset_to_pointer(str, ibus_attr->start_index) - str;
        uint32_t end   = g_utf8_offset_to_pointer(str, ibus_attr->end_index) - str;

        switch (ibus_attr->type) {
        case IBUS_ATTR_TYPE_UNDERLINE:
            type = TEXT_MODEL_PREEDIT_STYLE_TYPE_UNDERLINE;
            switch (ibus_attr->value) {
            case IBUS_ATTR_UNDERLINE_NONE:
                value = TEXT_MODEL_PREEDIT_UNDERLINE_TYPE_NONE;
                break;
            case IBUS_ATTR_UNDERLINE_SINGLE:
                value = TEXT_MODEL_PREEDIT_UNDERLINE_TYPE_SINGLE;
                break;
            case IBUS_ATTR_UNDERLINE_DOUBLE:
                value = TEXT_MODEL_PREEDIT_UNDERLINE_TYPE_DOUBLE;
                break;
            case IBUS_ATTR_UNDERLINE_LOW:
                value = TEXT_MODEL_PREEDIT_UNDERLINE_TYPE_LOW;
                break;
            case IBUS_ATTR_UNDERLINE_ERROR:
                value = TEXT_MODEL_PREEDIT_UNDERLINE_TYPE_ERROR;
                break;
            default:
                assert(false && "unknown ibus underline type");
            }
            break;

        case IBUS_ATTR_TYPE_FOREGROUND:
            type = TEXT_MODEL_PREEDIT_STYLE_TYPE_FOREGROUND;
            value = ibus_attr->value;
            break;
        case IBUS_ATTR_TYPE_BACKGROUND:
            type = TEXT_MODEL_PREEDIT_STYLE_TYPE_BACKGROUND;
            value = ibus_attr->value;
            break;
        default:
            assert(false && "unknown ibus attribute type");
        }
        input_method_context_preedit_styling(context->ime_context, type, value, start, end);
    }
}

static void
_update_preedit(struct ibus_ime_context *context)
{
    if (context->preedit_visible) {
        if (!context->preedit_started) {
            context->preedit_started = true;
        }
        _send_preedit(context);
    } else if (context->preedit_started) {
        input_method_context_preedit_string(context->ime_context, "", 0);
        context->preedit_started = false;
    }
}

static void
_context_update_preedit_text_cb (IBusInputContext        *ibus_context,
                                 IBusText                *text,
                                 gint                     cursor_pos,
                                 gboolean                 visible,
                                 struct ibus_ime_context *context)
{
    g_assert (IBUS_IS_INPUT_CONTEXT (ibus_context));
    g_assert (IBUS_IS_TEXT (text));
    g_assert (context);

    if (context->preedit_string) {
        free(context->preedit_string);
    }
    context->preedit_string = strdup(text->text);

    if (context->preedit_attrs) {
        g_object_unref (context->preedit_attrs);
    }

    g_object_ref(text->attrs);
    context->preedit_attrs = text->attrs;

    context->preedit_cursor = cursor_pos;
    context->preedit_visible = visible;

    _update_preedit (context);
}

static void
_context_show_preedit_text_cb (IBusInputContext        *ibus_context,
                               struct ibus_ime_context *context)
{
    g_assert (IBUS_IS_INPUT_CONTEXT (ibus_context));
    g_assert (context);

    context->preedit_visible = true;
    _update_preedit (context);
}

static void
_context_hide_preedit_text_cb (IBusInputContext        *ibus_context,
                               struct ibus_ime_context *context)
{
    g_assert (IBUS_IS_INPUT_CONTEXT (ibus_context));
    g_assert (context);

    context->preedit_visible = false;
    _update_preedit (context);
}

static void
_context_enabled_cb (IBusInputContext        *ibus_context,
                     struct ibus_ime_context *context)
{
    g_assert (IBUS_IS_INPUT_CONTEXT (ibus_context));
    g_assert (context);

    if (context->ime->active_context != context) {
        fprintf(stderr, "wut? enabled\n");
        return;
    }

    // Is there anything that needs to be done here?
}

static void
_context_disabled_cb (IBusInputContext        *ibus_context,
                      struct ibus_ime_context *context)
{
    g_assert (IBUS_IS_INPUT_CONTEXT (ibus_context));
    g_assert (context);

    if (context->ime->active_context != context) {
        fprintf(stderr, "wut? disabled\n");
        return;
    }

    reset_ibus_ime_context(context);
}

static void
_init_ibus_context (struct ibus_ime *ime, struct ibus_ime_context *context)
{
    context->ibus_context = ibus_bus_create_input_context (ime->bus, "wayland");
    assert(context->ibus_context && "ibus context couldn't be created");

    g_signal_connect (context->ibus_context, "commit-text",
                      G_CALLBACK (_context_commit_text_cb), context);
    g_signal_connect (context->ibus_context, "forward-key-event",
                      G_CALLBACK (_context_forward_key_event_cb), context);

    g_signal_connect (context->ibus_context, "update-preedit-text",
                      G_CALLBACK (_context_update_preedit_text_cb), context);
    g_signal_connect (context->ibus_context, "show-preedit-text",
                      G_CALLBACK (_context_show_preedit_text_cb), context);
    g_signal_connect (context->ibus_context, "hide-preedit-text",
                      G_CALLBACK (_context_hide_preedit_text_cb), context);
    g_signal_connect (context->ibus_context, "enabled",
                      G_CALLBACK (_context_enabled_cb), context);
    g_signal_connect (context->ibus_context, "disabled",
                      G_CALLBACK (_context_disabled_cb), context);

    // TODO add IBUS_CAP_SURROUNDING_TEXT once wayland/weston supports that
    ibus_input_context_set_capabilities (context->ibus_context,
                                         IBUS_CAP_FOCUS | IBUS_CAP_PREEDIT_TEXT);
}

static void
input_method_context_focus_in(void *data,
                              struct input_method_context *input_method_context)
{
    struct ibus_ime_context *context = data;
    struct ibus_ime *ime = context->ime;

    fprintf(stderr, "focus in\n");

    if (ime->active_context)
        fprintf(stderr, "WARNING: active context exists\n");

    ibus_input_context_focus_in(context->ibus_context);
    ime->active_context = context;
}

static void
input_method_context_focus_out(void *data,
                               struct input_method_context *input_method_context)
{
    struct ibus_ime_context *context = data;
    struct ibus_ime *ime = context->ime;

    fprintf(stderr, "focus out\n");

    ibus_input_context_focus_out(context->ibus_context);

    if (!ime->active_context) {
        fprintf(stderr, "WARNING: no active context\n");
        return;
    }

    if (ime->active_context != context)
        fprintf(stderr, "WARNING: active context exists\n");

    ime->active_context = NULL;
}

static void
input_method_context_reset(void *data,
                           struct input_method_context *input_method_context)
{
    struct ibus_ime_context *context = data;

    reset_ibus_ime_context(context);
}

static void
input_method_context_set_cursor_rectangle(void *data,
					  struct input_method_context *input_method_context,
					  int32_t x,
					  int32_t y,
					  int32_t width,
					  int32_t height)
{
	struct ibus_ime_context *context = data;

        fprintf(stderr, "cursor at %dx%d, size %dx%d\n", x, y, width, height);

        ibus_input_context_set_cursor_location(context->ibus_context,
                                               x, y, width, height);
}

static void
input_method_context_destroy_me(void *data,
                                struct input_method_context *input_method_context)
{
    struct ibus_ime_context *context = data;
    struct ibus_ime *ime = context->ime;

    fprintf(stderr, "destroy\n");

    ibus_proxy_destroy((IBusProxy *) context->ibus_context);

    if (context == ime->active_context) {
        fprintf(stderr, "WARNING: destroying active context\n");
        ime->active_context = NULL;
    }
    input_method_context_destroy(input_method_context);
    free(context);
}

static const
struct input_method_context_listener input_method_context_listener = {
    input_method_context_focus_in,
    input_method_context_focus_out,
    input_method_context_reset,
    input_method_context_set_cursor_rectangle,
    input_method_context_destroy_me,
};

static void
input_method_create_context(void *data,
                            struct input_method *input_method,
                            struct input_method_context *input_method_context)
{
    struct ibus_ime *ime = data;
    struct ibus_ime_context *context;

    fprintf(stderr, "create\n");

    context = calloc(1, sizeof *context);
    context->ime_context = input_method_context;
    context->ime = ime;
    input_method_context_add_listener(input_method_context,
                                      &input_method_context_listener,
                                      context);
    _init_ibus_context(ime, context);
}

static const struct input_method_listener input_method_listener = {
    input_method_create_context,
};

static void
input_method_keymap(void *data,
                    struct wl_keyboard *wl_keyboard,
                    uint32_t format,
                    int32_t fd,
                    uint32_t size)
{
    struct ibus_ime *ime = data;

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

static gboolean
process_key(struct ibus_ime *ime, uint32_t key, xkb_keysym_t sym,
            enum wl_keyboard_key_state state)
{
    struct ibus_ime_context *context = ime->active_context;
    guint32 key_state = 0;

    assert(context && "no active context");

    if (ime->modifiers & MOD_CONTROL_MASK)
        key_state |= IBUS_CONTROL_MASK;
    if (ime->modifiers & MOD_SHIFT_MASK)
        key_state |= IBUS_SHIFT_MASK;
    if (ime->modifiers & MOD_ALT_MASK)
        key_state |= IBUS_MOD1_MASK;
    if (state == WL_KEYBOARD_KEY_STATE_RELEASED)
        key_state |= IBUS_RELEASE_MASK;

    return ibus_input_context_process_key_event (
        context->ibus_context,
        sym,
        key - 8,
        key_state);
}

static void
input_method_key(void *data,
                 struct wl_keyboard *wl_keyboard,
                 uint32_t serial,
                 uint32_t time,
                 uint32_t key,
                 uint32_t state_w)
{
    struct ibus_ime *ime = data;
    struct ibus_ime_context *context = ime->active_context;

    assert(context && "no active context");

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
    // end of copied code

    gboolean processed = process_key(ime, key, sym, state);

    if (!processed) {
        // The key event could not be processed correctly.
        input_method_context_forward_key(context->ime_context, time, key, state_w);
    }
}

static void
input_method_modifiers(void *data,
                       struct wl_keyboard *wl_keyboard,
                       uint32_t serial,
                       uint32_t mods_depressed,
                       uint32_t mods_latched,
                       uint32_t mods_locked,
                       uint32_t group)
{
    struct ibus_ime *ime = data;

    // XXX: copied from window.c
    xkb_state_update_mask(ime->xkb.state, mods_depressed, mods_latched,
                          mods_locked, 0, 0, group);
}

static const struct wl_keyboard_listener input_method_keyboard_listener = {
    input_method_keymap,
    NULL, /* enter */
    NULL, /* leave */
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
        ime->input_method_keyboard =
            input_method_request_keyboard(ime->input_method);
        wl_keyboard_add_listener(ime->input_method_keyboard,
                                 &input_method_keyboard_listener,
                                 ime);
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
_init_ibus_bus (struct ibus_ime *ime)
{
    if (ime->bus != NULL)
        return;

    ibus_init ();

    ime->bus = ibus_bus_new ();

    g_signal_connect (ime->bus, "disconnected",
                      G_CALLBACK (_bus_disconnected_cb), NULL);
}

gboolean
ibus_source_prepare(GSource *source,
                    gint *timeout)
{
    // This is a "poll source". Wait indefinitely.
    *timeout = -1;
    return FALSE;
}

gboolean
ibus_source_check(GSource *source)
{
    struct ibus_ime_source *ime_source = (struct ibus_ime_source *) source;
    if (ime_source->poll_fd.revents & G_IO_ERR) {
        fprintf(stderr, "Error while polling\n");
        exit(1);
    }
    if (!(ime_source->poll_fd.revents & G_IO_IN))
        return FALSE;

    struct epoll_event ep;

    // Just to test if at least one fd is readable
    int count = epoll_wait(ime_source->poll_fd.fd,
                           &ep, 1, 0);
    if (count < 0) {
        perror("epoll");
        abort();
    }

    return count == 1 ? TRUE : FALSE;
}

gboolean
ibus_source_dispatch(GSource *source,
                     GSourceFunc callback,
                     gpointer user_data)
{
    struct ibus_ime_source *ime_source = (struct ibus_ime_source *) source;

    display_iteration_epoll(ime_source->ime->display, 0);
    display_iteration_deferred(ime_source->ime->display);

    // FIXME: what's the significance of the return value?
    return TRUE;
}

static GSourceFuncs ibus_source_funcs = {
    ibus_source_prepare,
    ibus_source_check,
    ibus_source_dispatch,
    NULL // finalize is not needed
};

static void
run_main_loop (struct ibus_ime *ime)
{
    GSource *source = g_source_new(&ibus_source_funcs,
                                   sizeof(struct ibus_ime_source));
    struct ibus_ime_source *ibus_ime_source = (struct ibus_ime_source *) source;
    ibus_ime_source->ime = ime;

    /* This works because (quote from epoll (7)):
     *
     * Q3  Is the epoll file descriptor itself poll/epoll/selectable?
     * A3  Yes. If an epoll file descriptor has events waiting then it
     *     will indicate as being readable.
     */
    ibus_ime_source->poll_fd.fd = display_get_epoll_fd(ime->display);
    ibus_ime_source->poll_fd.events = G_IO_IN;

    g_source_add_poll(source, &ibus_ime_source->poll_fd);

    GMainContext *context = g_main_context_default();
    g_source_attach(source, context);

    // run the deferred display tasks before starting the main loop. this is
    // needed to bootstrap the process (run global handlers and registration).
    display_iteration_deferred(ime->display);

    GMainLoop *main_loop = g_main_loop_new(context, TRUE);
    g_main_loop_run(main_loop);

    g_main_loop_unref(main_loop);
    g_source_unref(source);
}

int
main(int argc, char *argv[])
{
    struct ibus_ime ime;

    g_type_init();

    memset(&ime, 0, sizeof ime);

    _init_ibus_bus(&ime);

    if (!ibus_bus_is_connected (ime.bus)) {
        g_warning ("Can not connect to ibus daemon");
        exit (EXIT_FAILURE);
    }

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

    run_main_loop(&ime);

    g_object_unref (ime.bus);
    xkb_state_unref(ime.xkb.state);
    xkb_map_unref(ime.xkb.keymap);

    return 0;
}
