#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <wlr/config.h>
#include <wlr/types/wlr_box.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_gtk_primary_selection.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle.h>
#include <wlr/types/wlr_input_inhibitor.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_tablet_v2.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell_v6.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include "rootston/layers.h"
#include "rootston/seat.h"
#include "rootston/server.h"
#include "rootston/view.h"
#include "rootston/virtual_keyboard.h"
#include "rootston/xcursor.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"

static bool view_at(struct roots_view *view, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	if (view->wlr_surface == NULL) {
		return false;
	}

	double view_sx = lx - view->box.x;
	double view_sy = ly - view->box.y;
	rotate_child_position(&view_sx, &view_sy, 0, 0,
		view->box.width, view->box.height, -view->rotation);

	double _sx, _sy;
	struct wlr_surface *_surface = NULL;
	switch (view->type) {
	case ROOTS_XDG_SHELL_V6_VIEW:;
		struct roots_xdg_surface_v6 *xdg_surface_v6 =
			roots_xdg_surface_v6_from_view(view);
		_surface = wlr_xdg_surface_v6_surface_at(xdg_surface_v6->xdg_surface_v6,
			view_sx, view_sy, &_sx, &_sy);
		break;
	case ROOTS_XDG_SHELL_VIEW:;
		struct roots_xdg_surface *xdg_surface =
			roots_xdg_surface_from_view(view);
		_surface = wlr_xdg_surface_surface_at(xdg_surface->xdg_surface,
			view_sx, view_sy, &_sx, &_sy);
		break;
#if WLR_HAS_XWAYLAND
	case ROOTS_XWAYLAND_VIEW:
		_surface = wlr_surface_surface_at(view->wlr_surface,
			view_sx, view_sy, &_sx, &_sy);
		break;
#endif
	}
	if (_surface != NULL) {
		*sx = _sx;
		*sy = _sy;
		*surface = _surface;
		return true;
	}

	if (view_get_deco_part(view, view_sx, view_sy)) {
		*sx = view_sx;
		*sy = view_sy;
		*surface = NULL;
		return true;
	}

	return false;
}

static struct roots_view *desktop_view_at(struct roots_desktop *desktop,
		double lx, double ly, struct wlr_surface **surface,
		double *sx, double *sy) {
	struct roots_view *view;
	wl_list_for_each(view, &desktop->views, link) {
		if (view_at(view, lx, ly, surface, sx, sy)) {
			return view;
		}
	}
	return NULL;
}

static struct wlr_surface *layer_surface_at(struct roots_output *output,
		struct wl_list *layer, double ox, double oy, double *sx, double *sy) {
	struct roots_layer_surface *roots_surface;
	wl_list_for_each_reverse(roots_surface, layer, link) {
		double _sx = ox - roots_surface->geo.x;
		double _sy = oy - roots_surface->geo.y;

		struct wlr_surface *sub = wlr_layer_surface_v1_surface_at(
			roots_surface->layer_surface, _sx, _sy, sx, sy);

		if (sub) {
			return sub;
		}
	}

	return NULL;
}

struct wlr_surface *desktop_surface_at(struct roots_desktop *desktop,
		double lx, double ly, double *sx, double *sy,
		struct roots_view **view) {
	struct wlr_surface *surface = NULL;
	struct wlr_output *wlr_output =
		wlr_output_layout_output_at(desktop->layout, lx, ly);
	struct roots_output *roots_output = NULL;
	double ox = lx, oy = ly;
	if (view) {
		*view = NULL;
	}

	if (wlr_output) {
		roots_output = wlr_output->data;
		wlr_output_layout_output_coords(desktop->layout, wlr_output, &ox, &oy);

		if ((surface = layer_surface_at(roots_output,
					&roots_output->layers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY],
					ox, oy, sx, sy))) {
			return surface;
		}

		struct roots_output *output =
			desktop_output_from_wlr_output(desktop, wlr_output);
		if (output != NULL && output->fullscreen_view != NULL) {
			if (view_at(output->fullscreen_view, lx, ly, &surface, sx, sy)) {
				return surface;
			} else {
				return NULL;
			}
		}

		if ((surface = layer_surface_at(roots_output,
					&roots_output->layers[ZWLR_LAYER_SHELL_V1_LAYER_TOP],
					ox, oy, sx, sy))) {
			return surface;
		}
	}

	struct roots_view *_view;
	if ((_view = desktop_view_at(desktop, lx, ly, &surface, sx, sy))) {
		if (view) {
			*view = _view;
		}
		return surface;
	}

	if (wlr_output) {
		if ((surface = layer_surface_at(roots_output,
					&roots_output->layers[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM],
					ox, oy, sx, sy))) {
			return surface;
		}
		if ((surface = layer_surface_at(roots_output,
					&roots_output->layers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND],
					ox, oy, sx, sy))) {
			return surface;
		}
	}
	return NULL;
}

static void handle_layout_change(struct wl_listener *listener, void *data) {
	struct roots_desktop *desktop =
		wl_container_of(listener, desktop, layout_change);

	struct wlr_output *center_output =
		wlr_output_layout_get_center_output(desktop->layout);
	if (center_output == NULL) {
		return;
	}

	struct wlr_box *center_output_box =
		wlr_output_layout_get_box(desktop->layout, center_output);
	double center_x = center_output_box->x + center_output_box->width/2;
	double center_y = center_output_box->y + center_output_box->height/2;

	struct roots_view *view;
	wl_list_for_each(view, &desktop->views, link) {
		struct wlr_box box;
		view_get_box(view, &box);

		if (wlr_output_layout_intersects(desktop->layout, NULL, &box)) {
			continue;
		}

		view_move(view, center_x - box.width/2, center_y - box.height/2);
	}
}

static void input_inhibit_activate(struct wl_listener *listener, void *data) {
	struct roots_desktop *desktop = wl_container_of(
			listener, desktop, input_inhibit_activate);
	struct roots_seat *seat;
	wl_list_for_each(seat, &desktop->server->input->seats, link) {
		roots_seat_set_exclusive_client(seat,
				desktop->input_inhibit->active_client);
	}
}

static void input_inhibit_deactivate(struct wl_listener *listener, void *data) {
	struct roots_desktop *desktop = wl_container_of(
			listener, desktop, input_inhibit_deactivate);
	struct roots_seat *seat;
	wl_list_for_each(seat, &desktop->server->input->seats, link) {
		roots_seat_set_exclusive_client(seat, NULL);
	}
}

static void handle_constraint_destroy(struct wl_listener *listener,
		void *data) {
	struct roots_pointer_constraint *constraint =
		wl_container_of(listener, constraint, destroy);
	struct wlr_pointer_constraint_v1 *wlr_constraint = data;
	struct roots_seat *seat = wlr_constraint->seat->data;

	wl_list_remove(&constraint->destroy.link);

	if (seat->cursor->active_constraint == wlr_constraint) {
		wl_list_remove(&seat->cursor->constraint_commit.link);
		wl_list_init(&seat->cursor->constraint_commit.link);
		seat->cursor->active_constraint = NULL;

		if (wlr_constraint->current.committed &
				WLR_POINTER_CONSTRAINT_V1_STATE_CURSOR_HINT &&
				seat->cursor->pointer_view) {
			double sx = wlr_constraint->current.cursor_hint.x;
			double sy = wlr_constraint->current.cursor_hint.y;

			struct roots_view *view = seat->cursor->pointer_view->view;
			rotate_child_position(&sx, &sy, 0, 0, view->box.width, view->box.height,
				view->rotation);
			double lx = view->box.x + sx;
			double ly = view->box.y + sy;

			wlr_cursor_warp(seat->cursor->cursor, NULL, lx, ly);
		}
	}

	free(constraint);
}

static void handle_pointer_constraint(struct wl_listener *listener,
		void *data) {
	struct wlr_pointer_constraint_v1 *wlr_constraint = data;
	struct roots_seat *seat = wlr_constraint->seat->data;

	struct roots_pointer_constraint *constraint =
		calloc(1, sizeof(struct roots_pointer_constraint));
	constraint->constraint = wlr_constraint;

	constraint->destroy.notify = handle_constraint_destroy;
	wl_signal_add(&wlr_constraint->events.destroy, &constraint->destroy);

	double sx, sy;
	struct wlr_surface *surface = desktop_surface_at(
		seat->input->server->desktop,
		seat->cursor->cursor->x, seat->cursor->cursor->y, &sx, &sy, NULL);

	if (surface == wlr_constraint->surface) {
		assert(!seat->cursor->active_constraint);
		roots_cursor_constrain(seat->cursor, wlr_constraint, sx, sy);
	}
}

struct roots_desktop *desktop_create(struct roots_server *server,
		struct roots_config *config) {
	wlr_log(WLR_DEBUG, "Initializing roots desktop");

	struct roots_desktop *desktop = calloc(1, sizeof(struct roots_desktop));
	if (desktop == NULL) {
		return NULL;
	}

	wl_list_init(&desktop->views);
	wl_list_init(&desktop->outputs);

	desktop->new_output.notify = handle_new_output;
	wl_signal_add(&server->backend->events.new_output, &desktop->new_output);

	desktop->server = server;
	desktop->config = config;

	desktop->layout = wlr_output_layout_create();
	wlr_xdg_output_manager_v1_create(server->wl_display, desktop->layout);
	desktop->layout_change.notify = handle_layout_change;
	wl_signal_add(&desktop->layout->events.change, &desktop->layout_change);

	desktop->compositor = wlr_compositor_create(server->wl_display,
		server->renderer);

	desktop->xdg_shell_v6 = wlr_xdg_shell_v6_create(server->wl_display);
	wl_signal_add(&desktop->xdg_shell_v6->events.new_surface,
		&desktop->xdg_shell_v6_surface);
	desktop->xdg_shell_v6_surface.notify = handle_xdg_shell_v6_surface;

	desktop->xdg_shell = wlr_xdg_shell_create(server->wl_display);
	wl_signal_add(&desktop->xdg_shell->events.new_surface,
		&desktop->xdg_shell_surface);
	desktop->xdg_shell_surface.notify = handle_xdg_shell_surface;

	desktop->layer_shell = wlr_layer_shell_v1_create(server->wl_display);
	wl_signal_add(&desktop->layer_shell->events.new_surface,
		&desktop->layer_shell_surface);
	desktop->layer_shell_surface.notify = handle_layer_shell_surface;

	desktop->tablet_v2 = wlr_tablet_v2_create(server->wl_display);

	const char *cursor_theme = NULL;
#if WLR_HAS_XWAYLAND
	const char *cursor_default = ROOTS_XCURSOR_DEFAULT;
#endif
	struct roots_cursor_config *cc =
		roots_config_get_cursor(config, ROOTS_CONFIG_DEFAULT_SEAT_NAME);
	if (cc != NULL) {
		cursor_theme = cc->theme;
#if WLR_HAS_XWAYLAND
		if (cc->default_image != NULL) {
			cursor_default = cc->default_image;
		}
#endif
	}

	char cursor_size_fmt[16];
	snprintf(cursor_size_fmt, sizeof(cursor_size_fmt),
		"%d", ROOTS_XCURSOR_SIZE);
	setenv("XCURSOR_SIZE", cursor_size_fmt, 1);
	if (cursor_theme != NULL) {
		setenv("XCURSOR_THEME", cursor_theme, 1);
	}

#if WLR_HAS_XWAYLAND
	desktop->xcursor_manager = wlr_xcursor_manager_create(cursor_theme,
		ROOTS_XCURSOR_SIZE);
	if (desktop->xcursor_manager == NULL) {
		wlr_log(WLR_ERROR, "Cannot create XCursor manager for theme %s",
			cursor_theme);
		free(desktop);
		return NULL;
	}

	if (config->xwayland) {
		desktop->xwayland = wlr_xwayland_create(server->wl_display,
			desktop->compositor, config->xwayland_lazy);
		wl_signal_add(&desktop->xwayland->events.new_surface,
			&desktop->xwayland_surface);
		desktop->xwayland_surface.notify = handle_xwayland_surface;

		setenv("DISPLAY", desktop->xwayland->display_name, true);

		if (wlr_xcursor_manager_load(desktop->xcursor_manager, 1)) {
			wlr_log(WLR_ERROR, "Cannot load XWayland XCursor theme");
		}
		struct wlr_xcursor *xcursor = wlr_xcursor_manager_get_xcursor(
			desktop->xcursor_manager, cursor_default, 1);
		if (xcursor != NULL) {
			struct wlr_xcursor_image *image = xcursor->images[0];
			wlr_xwayland_set_cursor(desktop->xwayland, image->buffer,
				image->width * 4, image->width, image->height, image->hotspot_x,
				image->hotspot_y);
		}
	}
#endif

	desktop->gamma_control_manager_v1 = wlr_gamma_control_manager_v1_create(
		server->wl_display);
	desktop->export_dmabuf_manager_v1 =
		wlr_export_dmabuf_manager_v1_create(server->wl_display);
	desktop->server_decoration_manager =
		wlr_server_decoration_manager_create(server->wl_display);
	wlr_server_decoration_manager_set_default_mode(
		desktop->server_decoration_manager,
		WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT);
	desktop->idle = wlr_idle_create(server->wl_display);
	desktop->idle_inhibit = wlr_idle_inhibit_v1_create(server->wl_display);
	desktop->primary_selection_device_manager =
		wlr_gtk_primary_selection_device_manager_create(server->wl_display);
	desktop->input_inhibit =
		wlr_input_inhibit_manager_create(server->wl_display);
	desktop->input_inhibit_activate.notify = input_inhibit_activate;
	wl_signal_add(&desktop->input_inhibit->events.activate,
		&desktop->input_inhibit_activate);
	desktop->input_inhibit_deactivate.notify = input_inhibit_deactivate;
	wl_signal_add(&desktop->input_inhibit->events.deactivate,
		&desktop->input_inhibit_deactivate);

	desktop->input_method =
		wlr_input_method_manager_v2_create(server->wl_display);

	desktop->text_input = wlr_text_input_manager_v3_create(server->wl_display);

	desktop->virtual_keyboard = wlr_virtual_keyboard_manager_v1_create(
		server->wl_display);
	wl_signal_add(&desktop->virtual_keyboard->events.new_virtual_keyboard,
		&desktop->virtual_keyboard_new);
	desktop->virtual_keyboard_new.notify = handle_virtual_keyboard;

	desktop->screencopy = wlr_screencopy_manager_v1_create(server->wl_display);

	desktop->xdg_decoration_manager =
		wlr_xdg_decoration_manager_v1_create(server->wl_display);
	wl_signal_add(&desktop->xdg_decoration_manager->events.new_toplevel_decoration,
		&desktop->xdg_toplevel_decoration);
	desktop->xdg_toplevel_decoration.notify = handle_xdg_toplevel_decoration;

	desktop->pointer_constraints =
		wlr_pointer_constraints_v1_create(server->wl_display);
	desktop->pointer_constraint.notify = handle_pointer_constraint;
	wl_signal_add(&desktop->pointer_constraints->events.new_constraint,
		&desktop->pointer_constraint);

	desktop->presentation =
		wlr_presentation_create(server->wl_display, server->backend);
	desktop->foreign_toplevel_manager_v1 =
		wlr_foreign_toplevel_manager_v1_create(server->wl_display);
	desktop->relative_pointer_manager =
		wlr_relative_pointer_manager_v1_create(server->wl_display);
	desktop->pointer_gestures =
		wlr_pointer_gestures_v1_create(server->wl_display);

        wlr_data_control_manager_v1_create(server->wl_display);

	return desktop;
}

void desktop_destroy(struct roots_desktop *desktop) {
	// TODO
}

struct roots_output *desktop_output_from_wlr_output(
		struct roots_desktop *desktop, struct wlr_output *wlr_output) {
	struct roots_output *output;
	wl_list_for_each(output, &desktop->outputs, link) {
		if (output->wlr_output == wlr_output) {
			return output;
		}
	}
	return NULL;
}
