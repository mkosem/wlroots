#define _XOPEN_SOURCE 700
#include <assert.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <errno.h>
#include <gbm.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <wayland-server.h>
#include <wayland-util.h>
#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/gles2.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/util/log.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "backend/drm/drm.h"
#include "backend/drm/iface.h"
#include "backend/drm/util.h"
#include "util/signal.h"

bool check_drm_features(struct wlr_drm_backend *drm) {
	uint64_t cap;
	if (drm->parent) {
		if (drmGetCap(drm->fd, DRM_CAP_PRIME, &cap) ||
				!(cap & DRM_PRIME_CAP_IMPORT)) {
			wlr_log(WLR_ERROR,
				"PRIME import not supported on secondary GPU");
			return false;
		}

		if (drmGetCap(drm->parent->fd, DRM_CAP_PRIME, &cap) ||
				!(cap & DRM_PRIME_CAP_EXPORT)) {
			wlr_log(WLR_ERROR,
				"PRIME export not supported on primary GPU");
			return false;
		}
	}

	if (drmSetClientCap(drm->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)) {
		wlr_log(WLR_ERROR, "DRM universal planes unsupported");
		return false;
	}

	if (drmGetCap(drm->fd, DRM_CAP_CRTC_IN_VBLANK_EVENT, &cap) || !cap) {
		wlr_log(WLR_ERROR, "DRM_CRTC_IN_VBLANK_EVENT unsupported");
		return false;
	}

	const char *no_atomic = getenv("WLR_DRM_NO_ATOMIC");
	if (no_atomic && strcmp(no_atomic, "1") == 0) {
		wlr_log(WLR_DEBUG,
			"WLR_DRM_NO_ATOMIC set, forcing legacy DRM interface");
		drm->iface = &legacy_iface;
	} else if (drmSetClientCap(drm->fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
		wlr_log(WLR_DEBUG,
			"Atomic modesetting unsupported, using legacy DRM interface");
		drm->iface = &legacy_iface;
	} else {
		wlr_log(WLR_DEBUG, "Using atomic DRM interface");
		drm->iface = &atomic_iface;
	}

	int ret = drmGetCap(drm->fd, DRM_CAP_TIMESTAMP_MONOTONIC, &cap);
	drm->clock = (ret == 0 && cap == 1) ? CLOCK_MONOTONIC : CLOCK_REALTIME;

	ret = drmGetCap(drm->fd, DRM_CAP_ADDFB2_MODIFIERS, &cap);
	drm->addfb2_modifiers = ret == 0 && cap == 1;

	return true;
}

static bool add_plane(struct wlr_drm_backend *drm,
		struct wlr_drm_crtc *crtc, drmModePlane *drm_plane,
		uint32_t type, union wlr_drm_plane_props *props) {
	assert(!(type == DRM_PLANE_TYPE_PRIMARY && crtc->primary));

	if (type == DRM_PLANE_TYPE_CURSOR && crtc->cursor) {
		return true;
	}

	struct wlr_drm_plane *p = calloc(1, sizeof(*p));
	if (!p) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return false;
	}

	p->type = type;
	p->id = drm_plane->plane_id;
	p->props = *props;

	// Choose an RGB format for the plane
	uint32_t rgb_format = DRM_FORMAT_INVALID;
	for (size_t j = 0; j < drm_plane->count_formats; ++j) {
		uint32_t fmt = drm_plane->formats[j];
		wlr_drm_format_set_add(&p->formats, fmt, DRM_FORMAT_MOD_INVALID);

		if (fmt == DRM_FORMAT_ARGB8888) {
			// Prefer formats with alpha channel
			rgb_format = fmt;
			break;
		} else if (fmt == DRM_FORMAT_XRGB8888) {
			rgb_format = fmt;
		}
	}
	p->drm_format = rgb_format;

	if (p->props.in_formats) {
		uint64_t blob_id;
		if (!get_drm_prop(drm->fd, p->id, p->props.in_formats, &blob_id)) {
			wlr_log(WLR_ERROR, "Failed to read IN_FORMATS property");
			goto error;
		}

		drmModePropertyBlobRes *blob = drmModeGetPropertyBlob(drm->fd, blob_id);
		if (!blob) {
			wlr_log(WLR_ERROR, "Failed to read IN_FORMATS blob");
			goto error;
		}

		struct drm_format_modifier_blob *data = blob->data;
		uint32_t *fmts = (uint32_t *)((char *)data + data->formats_offset);
		struct drm_format_modifier *mods = (struct drm_format_modifier *)
			((char *)data + data->modifiers_offset);
		for (uint32_t i = 0; i < data->count_modifiers; ++i) {
			for (int j = 0; j < 64; ++j) {
				if (mods[i].formats & ((uint64_t)1 << j)) {
					wlr_drm_format_set_add(&p->formats,
						fmts[j + mods[i].offset], mods[i].modifier);
				}
			}
		}

		drmModeFreePropertyBlob(blob);
	}

	switch (type) {
	case DRM_PLANE_TYPE_PRIMARY:
		crtc->primary = p;
		break;
	case DRM_PLANE_TYPE_CURSOR:
		crtc->cursor = p;
		break;
	default:
		abort();
	}

	return true;

error:
	free(p);
	return false;
}

static bool init_planes(struct wlr_drm_backend *drm) {
	drmModePlaneRes *plane_res = drmModeGetPlaneResources(drm->fd);
	if (!plane_res) {
		wlr_log_errno(WLR_ERROR, "Failed to get DRM plane resources");
		return false;
	}

	wlr_log(WLR_INFO, "Found %"PRIu32" DRM planes", plane_res->count_planes);

	for (uint32_t i = 0; i < plane_res->count_planes; ++i) {
		uint32_t id = plane_res->planes[i];

		drmModePlane *plane = drmModeGetPlane(drm->fd, id);
		if (!plane) {
			wlr_log_errno(WLR_ERROR, "Failed to get DRM plane");
			goto error;
		}

		union wlr_drm_plane_props props = {0};
		if (!get_drm_plane_props(drm->fd, id, &props)) {
			drmModeFreePlane(plane);
			goto error;
		}

		uint64_t type;
		if (!get_drm_prop(drm->fd, id, props.type, &type)) {
			drmModeFreePlane(plane);
			goto error;
		}

		/*
		 * This is a very naive implementation of the plane matching
		 * logic. Primary and cursor planes should only work on a
		 * single CRTC, and this should be perfectly adequate, but
		 * overlay planes can potentially work with multiple CRTCs,
		 * meaning this could return inefficent/skewed results.
		 *
		 * However, we don't really care about overlay planes, as we
		 * don't support them yet. We only bother to keep basic
		 * tracking of them for DRM lease clients.
		 *
		 * possible_crtcs is a bitmask of crtcs, where each bit is an
		 * index into drmModeRes.crtcs. So if bit 0 is set (ffs starts
		 * counting from 1), crtc 0 is possible.
		 */
		int crtc_bit = ffs(plane->possible_crtcs) - 1;

		// This would be a kernel bug
		assert(crtc_bit >= 0 && (size_t)crtc_bit < drm->num_crtcs);

		struct wlr_drm_crtc *crtc = &drm->crtcs[crtc_bit];

		if (type == DRM_PLANE_TYPE_OVERLAY) {
			uint32_t *tmp = realloc(crtc->overlays,
				sizeof(*crtc->overlays) * (crtc->num_overlays + 1));
			if (tmp) {
				crtc->overlays = tmp;
				crtc->overlays[crtc->num_overlays++] = id;
			}
			drmModeFreePlane(plane);
			continue;
		}

		if (!add_plane(drm, crtc, plane, type, &props)) {
			drmModeFreePlane(plane);
			goto error;
		}

		drmModeFreePlane(plane);
	}

	drmModeFreePlaneResources(plane_res);
	return true;

error:
	drmModeFreePlaneResources(plane_res);
	return false;
}

bool init_drm_resources(struct wlr_drm_backend *drm) {
	drmModeRes *res = drmModeGetResources(drm->fd);
	if (!res) {
		wlr_log_errno(WLR_ERROR, "Failed to get DRM resources");
		return false;
	}

	wlr_log(WLR_INFO, "Found %d DRM CRTCs", res->count_crtcs);

	drm->num_crtcs = res->count_crtcs;
	if (drm->num_crtcs == 0) {
		drmModeFreeResources(res);
		return true;
	}

	drm->crtcs = calloc(drm->num_crtcs, sizeof(drm->crtcs[0]));
	if (!drm->crtcs) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		goto error_res;
	}

	for (size_t i = 0; i < drm->num_crtcs; ++i) {
		struct wlr_drm_crtc *crtc = &drm->crtcs[i];
		crtc->id = res->crtcs[i];
		crtc->legacy_crtc = drmModeGetCrtc(drm->fd, crtc->id);
		get_drm_crtc_props(drm->fd, crtc->id, &crtc->props);
	}

	if (!init_planes(drm)) {
		goto error_crtcs;
	}

	drmModeFreeResources(res);

	return true;

error_crtcs:
	free(drm->crtcs);
error_res:
	drmModeFreeResources(res);
	return false;
}

void finish_drm_resources(struct wlr_drm_backend *drm) {
	if (!drm) {
		return;
	}

	for (size_t i = 0; i < drm->num_crtcs; ++i) {
		struct wlr_drm_crtc *crtc = &drm->crtcs[i];

		drmModeAtomicFree(crtc->atomic);
		drmModeFreeCrtc(crtc->legacy_crtc);

		if (crtc->mode_id) {
			drmModeDestroyPropertyBlob(drm->fd, crtc->mode_id);
		}
		if (crtc->gamma_lut) {
			drmModeDestroyPropertyBlob(drm->fd, crtc->gamma_lut);
		}

		free(crtc->gamma_table);

		if (crtc->primary) {
			wlr_drm_format_set_finish(&crtc->primary->formats);
			free(crtc->primary);
		}
		if (crtc->cursor) {
			wlr_drm_format_set_finish(&crtc->cursor->formats);
			free(crtc->cursor);
		}
		free(crtc->overlays);
	}

	free(drm->crtcs);
}

static struct wlr_drm_connector *get_drm_connector_from_output(
		struct wlr_output *wlr_output) {
	assert(wlr_output_is_drm(wlr_output));
	return (struct wlr_drm_connector *)wlr_output;
}

static bool drm_connector_attach_render(struct wlr_output *output,
		int *buffer_age) {
	struct wlr_drm_connector *conn = get_drm_connector_from_output(output);
	return make_drm_surface_current(&conn->crtc->primary->surf, buffer_age);
}

static bool drm_connector_commit(struct wlr_output *output) {
	struct wlr_drm_connector *conn = get_drm_connector_from_output(output);
	struct wlr_drm_backend *drm = get_drm_backend_from_backend(output->backend);
	if (!drm->session->active) {
		return false;
	}

	struct wlr_drm_crtc *crtc = conn->crtc;
	if (!crtc) {
		return false;
	}
	struct wlr_drm_plane *plane = crtc->primary;

	pixman_region32_t *damage = NULL;
	if (output->pending.committed & WLR_OUTPUT_STATE_DAMAGE) {
		damage = &output->pending.damage;
	}

	struct gbm_bo *bo;
	uint32_t fb_id = 0;
	assert(output->pending.committed & WLR_OUTPUT_STATE_BUFFER);
	switch (output->pending.buffer_type) {
	case WLR_OUTPUT_STATE_BUFFER_RENDER:
		bo = swap_drm_surface_buffers(&plane->surf, damage);
		if (bo == NULL) {
			wlr_log(WLR_ERROR, "swap_drm_surface_buffers failed");
			return false;
		}

		if (drm->parent) {
			bo = copy_drm_surface_mgpu(&plane->mgpu_surf, bo);
			if (bo == NULL) {
				wlr_log(WLR_ERROR, "copy_drm_surface_mgpu failed");
				return false;
			}
		}
		fb_id = get_fb_for_bo(bo, plane->drm_format, drm->addfb2_modifiers);
		if (fb_id == 0) {
			wlr_log(WLR_ERROR, "get_fb_for_bo failed");
			return false;
		}
		break;
	case WLR_OUTPUT_STATE_BUFFER_SCANOUT:
		bo = import_gbm_bo(&drm->renderer, &conn->pending_dmabuf);
		if (bo == NULL) {
			wlr_log(WLR_ERROR, "import_gbm_bo failed");
			return false;
		}

		fb_id = get_fb_for_bo(bo, gbm_bo_get_format(bo), drm->addfb2_modifiers);
		if (fb_id == 0) {
			wlr_log(WLR_ERROR, "get_fb_for_bo failed");
			return false;
		}
		break;
	}

	if (conn->pageflip_pending) {
		wlr_log(WLR_ERROR, "Skipping pageflip on output '%s'", conn->output.name);
		return false;
	}

	if (!drm->iface->crtc_pageflip(drm, conn, crtc, fb_id, NULL)) {
		return false;
	}

	conn->pageflip_pending = true;
	if (output->pending.buffer_type == WLR_OUTPUT_STATE_BUFFER_SCANOUT) {
		wlr_buffer_unref(conn->pending_buffer);
		conn->pending_buffer = wlr_buffer_ref(output->pending.buffer);
	}

	wlr_output_update_enabled(output, true);
	return true;
}

static void fill_empty_gamma_table(size_t size,
		uint16_t *r, uint16_t *g, uint16_t *b) {
	for (uint32_t i = 0; i < size; ++i) {
		uint16_t val = (uint32_t)0xffff * i / (size - 1);
		r[i] = g[i] = b[i] = val;
	}
}

static size_t drm_connector_get_gamma_size(struct wlr_output *output) {
	struct wlr_drm_connector *conn = get_drm_connector_from_output(output);
	struct wlr_drm_backend *drm = get_drm_backend_from_backend(output->backend);

	if (conn->crtc) {
		return drm->iface->crtc_get_gamma_size(drm, conn->crtc);
	}

	return 0;
}

bool set_drm_connector_gamma(struct wlr_output *output, size_t size,
		const uint16_t *r, const uint16_t *g, const uint16_t *b) {
	struct wlr_drm_connector *conn = get_drm_connector_from_output(output);
	struct wlr_drm_backend *drm = get_drm_backend_from_backend(output->backend);

	if (!conn->crtc) {
		return false;
	}

	bool reset = false;
	if (size == 0) {
		reset = true;
		size = drm_connector_get_gamma_size(output);
		if (size == 0) {
			return false;
		}
	}

	uint16_t *gamma_table = malloc(3 * size * sizeof(uint16_t));
	if (gamma_table == NULL) {
		wlr_log(WLR_ERROR, "Failed to allocate gamma table");
		return false;
	}
	uint16_t *_r = gamma_table;
	uint16_t *_g = gamma_table + size;
	uint16_t *_b = gamma_table + 2 * size;

	if (reset) {
		fill_empty_gamma_table(size, _r, _g, _b);
	} else {
		memcpy(_r, r, size * sizeof(uint16_t));
		memcpy(_g, g, size * sizeof(uint16_t));
		memcpy(_b, b, size * sizeof(uint16_t));
	}

	bool ok = drm->iface->crtc_set_gamma(drm, conn->crtc, size, _r, _g, _b);
	if (ok) {
		wlr_output_update_needs_frame(output);

		free(conn->crtc->gamma_table);
		conn->crtc->gamma_table = gamma_table;
		conn->crtc->gamma_table_size = size;
	} else {
		free(gamma_table);
	}
	return ok;
}

static bool drm_connector_export_dmabuf(struct wlr_output *output,
		struct wlr_dmabuf_attributes *attribs) {
	struct wlr_drm_connector *conn = get_drm_connector_from_output(output);
	struct wlr_drm_backend *drm = get_drm_backend_from_backend(output->backend);

	if (!drm->session->active) {
		return false;
	}

	struct wlr_drm_crtc *crtc = conn->crtc;
	if (!crtc) {
		return false;
	}
	struct wlr_drm_plane *plane = crtc->primary;
	struct wlr_drm_surface *surf = &plane->surf;

	return export_drm_bo(surf->back, attribs);
}

static void drm_connector_start_renderer(struct wlr_drm_connector *conn) {
	if (conn->state != WLR_DRM_CONN_CONNECTED) {
		return;
	}

	wlr_log(WLR_DEBUG, "Starting renderer on output '%s'", conn->output.name);

	struct wlr_drm_backend *drm =
		get_drm_backend_from_backend(conn->output.backend);
	struct wlr_drm_crtc *crtc = conn->crtc;
	if (!crtc) {
		return;
	}
	struct wlr_drm_plane *plane = crtc->primary;

	struct gbm_bo *bo = get_drm_surface_front(
		drm->parent ? &plane->mgpu_surf : &plane->surf);
	uint32_t fb_id = get_fb_for_bo(bo, plane->drm_format, drm->addfb2_modifiers);

	struct wlr_drm_mode *mode = (struct wlr_drm_mode *)conn->output.current_mode;
	if (drm->iface->crtc_pageflip(drm, conn, crtc, fb_id, &mode->drm_mode)) {
		conn->pageflip_pending = true;
		wlr_output_update_enabled(&conn->output, true);
	} else {
		wl_event_source_timer_update(conn->retry_pageflip,
			1000000.0f / conn->output.current_mode->refresh);
	}
}

static void realloc_crtcs(struct wlr_drm_backend *drm);

static void attempt_enable_needs_modeset(struct wlr_drm_backend *drm) {
	// Try to modeset any output that has a desired mode and a CRTC (ie. was
	// lacking a CRTC on last modeset)
	struct wlr_drm_connector *conn;
	wl_list_for_each(conn, &drm->outputs, link) {
		if (conn->state == WLR_DRM_CONN_NEEDS_MODESET &&
				conn->crtc != NULL && conn->desired_mode != NULL &&
				conn->desired_enabled) {
			wlr_log(WLR_DEBUG, "Output %s has a desired mode and a CRTC, "
				"attempting a modeset", conn->output.name);
			drm_connector_set_mode(&conn->output, conn->desired_mode);
		}
	}
}

bool enable_drm_connector(struct wlr_output *output, bool enable) {
	struct wlr_drm_connector *conn = get_drm_connector_from_output(output);
	struct wlr_drm_backend *drm = get_drm_backend_from_backend(output->backend);
	if (conn->state != WLR_DRM_CONN_CONNECTED
			&& conn->state != WLR_DRM_CONN_NEEDS_MODESET) {
		return false;
	}

	conn->desired_enabled = enable;

	if (enable && conn->crtc == NULL) {
		// Maybe we can steal a CRTC from a disabled output
		realloc_crtcs(drm);
	}

	bool ok = drm->iface->conn_enable(drm, conn, enable);
	if (!ok) {
		return false;
	}

	if (enable) {
		drm_connector_start_renderer(conn);
	} else {
		realloc_crtcs(drm);

		attempt_enable_needs_modeset(drm);
	}

	wlr_output_update_enabled(&conn->output, enable);
	return true;
}

static void drm_connector_cleanup(struct wlr_drm_connector *conn);

bool drm_connector_set_mode(struct wlr_output *output,
		struct wlr_output_mode *mode) {
	struct wlr_drm_connector *conn = get_drm_connector_from_output(output);
	struct wlr_drm_backend *drm = get_drm_backend_from_backend(output->backend);
	if (conn->crtc == NULL) {
		// Maybe we can steal a CRTC from a disabled output
		realloc_crtcs(drm);
	}
	if (conn->crtc == NULL) {
		wlr_log(WLR_ERROR, "Cannot modeset '%s': no CRTC for this connector",
			conn->output.name);
		// Save the desired mode for later, when we'll get a proper CRTC
		conn->desired_mode = mode;
		return false;
	}

	wlr_log(WLR_INFO, "Modesetting '%s' with '%ux%u@%u mHz'",
		conn->output.name, mode->width, mode->height, mode->refresh);

	if (!init_drm_plane_surfaces(conn->crtc->primary, drm,
			mode->width, mode->height, drm->renderer.gbm_format)) {
		wlr_log(WLR_ERROR, "Failed to initialize renderer for plane");
		return false;
	}

	conn->state = WLR_DRM_CONN_CONNECTED;
	conn->desired_mode = NULL;
	wlr_output_update_mode(&conn->output, mode);
	wlr_output_update_enabled(&conn->output, true);
	conn->desired_enabled = true;

	drm_connector_start_renderer(conn);

	// When switching VTs, the mode is not updated but the buffers become
	// invalid, so we need to manually damage the output here
	wlr_output_damage_whole(&conn->output);

	return true;
}

bool wlr_drm_connector_add_mode(struct wlr_output *output,
		const drmModeModeInfo *modeinfo) {
	struct wlr_drm_connector *conn = get_drm_connector_from_output(output);

	if (modeinfo->type != DRM_MODE_TYPE_USERDEF) {
		return false;
	}

	struct wlr_output_mode *wlr_mode;
	wl_list_for_each(wlr_mode, &conn->output.modes, link) {
		struct wlr_drm_mode *mode = (struct wlr_drm_mode *)wlr_mode;
		if (memcmp(&mode->drm_mode, modeinfo, sizeof(*modeinfo)) == 0) {
			return true;
		}
	}

	struct wlr_drm_mode *mode = calloc(1, sizeof(*mode));
	if (!mode) {
		return false;
	}
	memcpy(&mode->drm_mode, modeinfo, sizeof(*modeinfo));

	mode->wlr_mode.width = mode->drm_mode.hdisplay;
	mode->wlr_mode.height = mode->drm_mode.vdisplay;
	mode->wlr_mode.refresh = calculate_refresh_rate(modeinfo);

	wlr_log(WLR_INFO, "Registered custom mode "
			"%"PRId32"x%"PRId32"@%"PRId32,
			mode->wlr_mode.width, mode->wlr_mode.height,
			mode->wlr_mode.refresh);
	wl_list_insert(&conn->output.modes, &mode->wlr_mode.link);
	return true;
}

static bool drm_connector_set_cursor(struct wlr_output *output,
		struct wlr_texture *texture, int32_t scale,
		enum wl_output_transform transform,
		int32_t hotspot_x, int32_t hotspot_y, bool update_texture) {
	struct wlr_drm_connector *conn = get_drm_connector_from_output(output);
	struct wlr_drm_backend *drm = get_drm_backend_from_backend(output->backend);

	struct wlr_drm_crtc *crtc = conn->crtc;
	if (!crtc) {
		return false;
	}

	struct wlr_drm_plane *plane = crtc->cursor;
	if (!plane) {
		// We don't have a real cursor plane, so we make a fake one
		plane = calloc(1, sizeof(*plane));
		if (!plane) {
			wlr_log_errno(WLR_ERROR, "Allocation failed");
			return false;
		}
		crtc->cursor = plane;
	}

	if (!plane->surf.gbm) {
		int ret;
		uint64_t w, h;
		ret = drmGetCap(drm->fd, DRM_CAP_CURSOR_WIDTH, &w);
		w = ret ? 64 : w;
		ret = drmGetCap(drm->fd, DRM_CAP_CURSOR_HEIGHT, &h);
		h = ret ? 64 : h;


		if (!drm->parent) {
			if (!init_drm_surface(&plane->surf, &drm->renderer, w, h,
					drm->renderer.gbm_format, GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT)) {
				wlr_log(WLR_ERROR, "Cannot allocate cursor resources");
				return false;
			}
		} else {
			if (!init_drm_surface(&plane->surf, &drm->parent->renderer, w, h,
					drm->parent->renderer.gbm_format, GBM_BO_USE_LINEAR)) {
				wlr_log(WLR_ERROR, "Cannot allocate cursor resources");
				return false;
			}

			if (!init_drm_surface(&plane->mgpu_surf, &drm->renderer, w, h,
					drm->renderer.gbm_format, GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT)) {
				wlr_log(WLR_ERROR, "Cannot allocate cursor resources");
				return false;
			}
		}
	}

	wlr_matrix_projection(plane->matrix, plane->surf.width,
		plane->surf.height, output->transform);

	struct wlr_box hotspot = { .x = hotspot_x, .y = hotspot_y };
	wlr_box_transform(&hotspot, &hotspot,
		wlr_output_transform_invert(output->transform),
		plane->surf.width, plane->surf.height);

	if (plane->cursor_hotspot_x != hotspot.x ||
			plane->cursor_hotspot_y != hotspot.y) {
		// Update cursor hotspot
		conn->cursor_x -= hotspot.x - plane->cursor_hotspot_x;
		conn->cursor_y -= hotspot.y - plane->cursor_hotspot_y;
		plane->cursor_hotspot_x = hotspot.x;
		plane->cursor_hotspot_y = hotspot.y;

		if (!drm->iface->crtc_move_cursor(drm, conn->crtc, conn->cursor_x,
				conn->cursor_y)) {
			return false;
		}

		wlr_output_update_needs_frame(output);
	}

	if (!update_texture) {
		// Don't update cursor image
		return true;
	}

	plane->cursor_enabled = false;
	if (texture != NULL) {
		int width, height;
		wlr_texture_get_size(texture, &width, &height);
		width = width * output->scale / scale;
		height = height * output->scale / scale;

		if (width > (int)plane->surf.width || height > (int)plane->surf.height) {
			wlr_log(WLR_ERROR, "Cursor too large (max %dx%d)",
				(int)plane->surf.width, (int)plane->surf.height);
			return false;
		}

		make_drm_surface_current(&plane->surf, NULL);

		struct wlr_renderer *rend = plane->surf.renderer->wlr_rend;

		struct wlr_box cursor_box = { .width = width, .height = height };

		float matrix[9];
		wlr_matrix_project_box(matrix, &cursor_box, transform, 0, plane->matrix);

		wlr_renderer_begin(rend, plane->surf.width, plane->surf.height);
		wlr_renderer_clear(rend, (float[]){ 0.0, 0.0, 0.0, 0.0 });
		wlr_render_texture_with_matrix(rend, texture, matrix, 1.0);
		wlr_renderer_end(rend);

		swap_drm_surface_buffers(&plane->surf, NULL);

		plane->cursor_enabled = true;
	}

	if (!drm->session->active) {
		return true; // will be committed when session is resumed
	}

	struct gbm_bo *bo = plane->cursor_enabled ? plane->surf.back : NULL;
	if (bo && drm->parent) {
		bo = copy_drm_surface_mgpu(&plane->mgpu_surf, bo);
	}

	if (bo) {
		// workaround for nouveau
		// Buffers created with GBM_BO_USER_LINEAR are placed in NOUVEAU_GEM_DOMAIN_GART.
		// When the bo is attached to the cursor plane it is moved to NOUVEAU_GEM_DOMAIN_VRAM.
		// However, this does not wait for the render operations to complete, leaving an empty surface.
		// see https://bugs.freedesktop.org/show_bug.cgi?id=109631
		// The render operations can be waited for using:
		glFinish();
	}
	bool ok = drm->iface->crtc_set_cursor(drm, crtc, bo);
	if (ok) {
		wlr_output_update_needs_frame(output);
	}
	return ok;
}

static bool drm_connector_move_cursor(struct wlr_output *output,
		int x, int y) {
	struct wlr_drm_connector *conn = get_drm_connector_from_output(output);
	struct wlr_drm_backend *drm = get_drm_backend_from_backend(output->backend);
	if (!conn->crtc) {
		return false;
	}
	struct wlr_drm_plane *plane = conn->crtc->cursor;

	struct wlr_box box = { .x = x, .y = y };

	int width, height;
	wlr_output_transformed_resolution(output, &width, &height);

	enum wl_output_transform transform =
		wlr_output_transform_invert(output->transform);
	wlr_box_transform(&box, &box, transform, width, height);

	if (plane != NULL) {
		box.x -= plane->cursor_hotspot_x;
		box.y -= plane->cursor_hotspot_y;
	}

	conn->cursor_x = box.x;
	conn->cursor_y = box.y;

	if (!drm->session->active) {
		return true; // will be committed when session is resumed
	}

	bool ok = drm->iface->crtc_move_cursor(drm, conn->crtc, box.x, box.y);
	if (ok) {
		wlr_output_update_needs_frame(output);
	}
	return ok;
}

static bool drm_connector_schedule_frame(struct wlr_output *output) {
	struct wlr_drm_connector *conn = get_drm_connector_from_output(output);
	struct wlr_drm_backend *drm = get_drm_backend_from_backend(output->backend);
	if (!drm->session->active) {
		return false;
	}

	// We need to figure out where we are in the vblank cycle
	// TODO: try using drmWaitVBlank and fallback to pageflipping

	struct wlr_drm_crtc *crtc = conn->crtc;
	if (!crtc) {
		return false;
	}
	struct wlr_drm_plane *plane = crtc->primary;
	struct gbm_bo *bo = plane->surf.back;
	if (!bo) {
		// We haven't swapped buffers yet -- can't do a pageflip
		wlr_output_send_frame(output);
		return true;
	}
	if (drm->parent) {
		bo = copy_drm_surface_mgpu(&plane->mgpu_surf, bo);
	}

	if (conn->pageflip_pending) {
		wlr_log(WLR_ERROR, "Skipping pageflip on output '%s'",
			conn->output.name);
		return true;
	}

	uint32_t fb_id = get_fb_for_bo(bo, plane->drm_format, drm->addfb2_modifiers);
	if (!drm->iface->crtc_pageflip(drm, conn, crtc, fb_id, NULL)) {
		return false;
	}

	conn->pageflip_pending = true;
	wlr_output_update_enabled(output, true);
	return true;
}

static uint32_t strip_alpha_channel(uint32_t format) {
	switch (format) {
	case DRM_FORMAT_ARGB8888:
		return DRM_FORMAT_XRGB8888;
	default:
		return DRM_FORMAT_INVALID;
	}
}

static bool drm_connector_attach_buffer(struct wlr_output *output,
		struct wlr_buffer *buffer) {
	struct wlr_drm_connector *conn = get_drm_connector_from_output(output);
	struct wlr_drm_backend *drm = get_drm_backend_from_backend(output->backend);
	if (!drm->session->active) {
		return false;
	}

	struct wlr_drm_crtc *crtc = conn->crtc;
	if (!crtc) {
		return false;
	}

	struct wlr_dmabuf_attributes attribs;
	if (!wlr_buffer_get_dmabuf(buffer, &attribs)) {
		return false;
	}

	if (attribs.flags != 0) {
		return false;
	}
	if (attribs.width != output->width || attribs.height != output->height) {
		return false;
	}

	if (!wlr_drm_format_set_has(&crtc->primary->formats,
			attribs.format, attribs.modifier)) {
		// The format isn't supported by the plane. Try stripping the alpha
		// channel, if any.
		uint32_t format = strip_alpha_channel(attribs.format);
		if (format != DRM_FORMAT_INVALID && wlr_drm_format_set_has(
				&crtc->primary->formats, format, attribs.modifier)) {
			attribs.format = format;
		} else {
			return false;
		}
	}

	memcpy(&conn->pending_dmabuf, &attribs, sizeof(attribs));
	return true;
}

static void drm_connector_destroy(struct wlr_output *output) {
	struct wlr_drm_connector *conn = get_drm_connector_from_output(output);
	drm_connector_cleanup(conn);
	drmModeFreeCrtc(conn->old_crtc);
	wl_event_source_remove(conn->retry_pageflip);
	wl_list_remove(&conn->link);
	free(conn);
}

static const struct wlr_output_impl output_impl = {
	.enable = enable_drm_connector,
	.set_mode = drm_connector_set_mode,
	.set_cursor = drm_connector_set_cursor,
	.move_cursor = drm_connector_move_cursor,
	.destroy = drm_connector_destroy,
	.attach_render = drm_connector_attach_render,
	.commit = drm_connector_commit,
	.set_gamma = set_drm_connector_gamma,
	.get_gamma_size = drm_connector_get_gamma_size,
	.export_dmabuf = drm_connector_export_dmabuf,
	.schedule_frame = drm_connector_schedule_frame,
	.attach_buffer = drm_connector_attach_buffer,
};

bool wlr_output_is_drm(struct wlr_output *output) {
	return output->impl == &output_impl;
}

static int retry_pageflip(void *data) {
	struct wlr_drm_connector *conn = data;
	wlr_log(WLR_INFO, "%s: Retrying pageflip", conn->output.name);
	drm_connector_start_renderer(conn);
	return 0;
}

static const int32_t subpixel_map[] = {
	[DRM_MODE_SUBPIXEL_UNKNOWN] = WL_OUTPUT_SUBPIXEL_UNKNOWN,
	[DRM_MODE_SUBPIXEL_HORIZONTAL_RGB] = WL_OUTPUT_SUBPIXEL_HORIZONTAL_RGB,
	[DRM_MODE_SUBPIXEL_HORIZONTAL_BGR] = WL_OUTPUT_SUBPIXEL_HORIZONTAL_BGR,
	[DRM_MODE_SUBPIXEL_VERTICAL_RGB] = WL_OUTPUT_SUBPIXEL_VERTICAL_RGB,
	[DRM_MODE_SUBPIXEL_VERTICAL_BGR] = WL_OUTPUT_SUBPIXEL_VERTICAL_BGR,
	[DRM_MODE_SUBPIXEL_NONE] = WL_OUTPUT_SUBPIXEL_NONE,
};

static void dealloc_crtc(struct wlr_drm_connector *conn) {
	struct wlr_drm_backend *drm =
		get_drm_backend_from_backend(conn->output.backend);
	if (conn->crtc == NULL) {
		return;
	}

	wlr_log(WLR_DEBUG, "De-allocating CRTC %zu for output '%s'",
		conn->crtc - drm->crtcs, conn->output.name);

	set_drm_connector_gamma(&conn->output, 0, NULL, NULL, NULL);
	finish_drm_surface(&conn->crtc->primary->surf);
	finish_drm_surface(&conn->crtc->cursor->surf);

	drm->iface->conn_enable(drm, conn, false);

	conn->crtc = NULL;
}

static void realloc_crtcs(struct wlr_drm_backend *drm) {
	assert(drm->num_crtcs > 0);

	size_t num_outputs = wl_list_length(&drm->outputs);
	if (num_outputs == 0) {
		return;
	}

	wlr_log(WLR_DEBUG, "Reallocating CRTCs");

	struct wlr_drm_connector *connectors[num_outputs];
	uint32_t connector_constraints[num_outputs];
	uint32_t previous_match[drm->num_crtcs];
	uint32_t new_match[drm->num_crtcs];

	for (size_t i = 0; i < drm->num_crtcs; ++i) {
		previous_match[i] = UNMATCHED;
	}

	wlr_log(WLR_DEBUG, "State before reallocation:");
	size_t i = 0;
	struct wlr_drm_connector *conn;
	wl_list_for_each(conn, &drm->outputs, link) {
		connectors[i] = conn;

		wlr_log(WLR_DEBUG, "  '%s' crtc=%d state=%d desired_enabled=%d",
			conn->output.name,
			conn->crtc ? (int)(conn->crtc - drm->crtcs) : -1,
			conn->state, conn->desired_enabled);

		if (conn->crtc) {
			previous_match[conn->crtc - drm->crtcs] = i;
		}

		// Only search CRTCs for user-enabled outputs (that are already
		// connected or in need of a modeset)
		if ((conn->state == WLR_DRM_CONN_CONNECTED ||
				conn->state == WLR_DRM_CONN_NEEDS_MODESET) &&
				conn->desired_enabled) {
			connector_constraints[i] = conn->possible_crtc;
		} else {
			// Will always fail to match anything
			connector_constraints[i] = 0;
		}

		++i;
	}

	match_obj(num_outputs, connector_constraints,
		drm->num_crtcs, previous_match, new_match);

	// Converts our crtc=>connector result into a connector=>crtc one.
	ssize_t connector_match[num_outputs];
	for (size_t i = 0 ; i < num_outputs; ++i) {
		connector_match[i] = -1;
	}
	for (size_t i = 0; i < drm->num_crtcs; ++i) {
		if (new_match[i] != UNMATCHED) {
			connector_match[new_match[i]] = i;
		}
	}

	/*
	 * In the case that we add a new connector (hotplug) and we fail to
	 * match everything, we prefer to fail the new connector and keep all
	 * of the old mappings instead.
	 */
	for (size_t i = 0; i < num_outputs; ++i) {
		struct wlr_drm_connector *conn = connectors[i];
		if (conn->state == WLR_DRM_CONN_CONNECTED &&
				conn->desired_enabled &&
				connector_match[i] == -1) {
			wlr_log(WLR_DEBUG, "Could not match a CRTC for previously connected output; "
					"keeping old configuration");
			return;
		}
	}
	wlr_log(WLR_DEBUG, "State after reallocation:");

	// Apply new configuration
	for (size_t i = 0; i < num_outputs; ++i) {
		struct wlr_drm_connector *conn = connectors[i];
		bool prev_enabled = conn->crtc;

		wlr_log(WLR_DEBUG, "  '%s' crtc=%zd state=%d desired_enabled=%d",
			conn->output.name,
			connector_match[i],
			conn->state, conn->desired_enabled);

		// We don't need to change anything.
		if (prev_enabled && connector_match[i] == conn->crtc - drm->crtcs) {
			continue;
		}

		dealloc_crtc(conn);

		if (connector_match[i] == -1) {
			if (prev_enabled) {
				wlr_log(WLR_DEBUG, "Output has %s lost its CRTC",
					conn->output.name);
				conn->state = WLR_DRM_CONN_NEEDS_MODESET;
				wlr_output_update_enabled(&conn->output, false);
				conn->desired_mode = conn->output.current_mode;
				wlr_output_update_mode(&conn->output, NULL);
			}
			continue;
		}

		conn->crtc = &drm->crtcs[connector_match[i]];

		// Only realloc buffers if we have actually been modeset
		if (conn->state != WLR_DRM_CONN_CONNECTED) {
			continue;
		}

		struct wlr_output_mode *mode = conn->output.current_mode;

		if (!init_drm_plane_surfaces(conn->crtc->primary, drm,
				mode->width, mode->height, drm->renderer.gbm_format)) {
			wlr_log(WLR_ERROR, "Failed to initialize renderer for plane");
			drm_connector_cleanup(conn);
			break;
		}

		drm_connector_start_renderer(conn);

		wlr_output_damage_whole(&conn->output);
	}
}

static uint32_t get_possible_crtcs(int fd, drmModeRes *res,
		drmModeConnector *conn, bool is_mst) {
	uint32_t ret = 0;

	for (int i = 0; i < conn->count_encoders; ++i) {
		drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[i]);
		if (!enc) {
			continue;
		}

		ret |= enc->possible_crtcs;

		drmModeFreeEncoder(enc);
	}

	// Sometimes DP MST connectors report no encoders, so we'll loop though
	// all of the encoders of the MST type instead.
	// TODO: See if there is a better solution.

	if (!is_mst || ret) {
		return ret;
	}

	for (int i = 0; i < res->count_encoders; ++i) {
		drmModeEncoder *enc = drmModeGetEncoder(fd, res->encoders[i]);
		if (!enc) {
			continue;
		}

		if (enc->encoder_type == DRM_MODE_ENCODER_DPMST) {
			ret |= enc->possible_crtcs;
		}

		drmModeFreeEncoder(enc);
	}

	return ret;
}

void scan_drm_connectors(struct wlr_drm_backend *drm) {
	/*
	 * This GPU is not really a modesetting device.
	 * It's just being used as a renderer.
	 */
	if (drm->num_crtcs == 0) {
		return;
	}

	wlr_log(WLR_INFO, "Scanning DRM connectors");

	drmModeRes *res = drmModeGetResources(drm->fd);
	if (!res) {
		wlr_log_errno(WLR_ERROR, "Failed to get DRM resources");
		return;
	}

	size_t seen_len = wl_list_length(&drm->outputs);
	// +1 so length can never be 0, which is undefined behaviour.
	// Last element isn't used.
	bool seen[seen_len + 1];
	memset(seen, false, sizeof(seen));
	size_t new_outputs_len = 0;
	struct wlr_drm_connector *new_outputs[res->count_connectors + 1];

	for (int i = 0; i < res->count_connectors; ++i) {
		drmModeConnector *drm_conn = drmModeGetConnector(drm->fd,
			res->connectors[i]);
		if (!drm_conn) {
			wlr_log_errno(WLR_ERROR, "Failed to get DRM connector");
			continue;
		}
		drmModeEncoder *curr_enc = drmModeGetEncoder(drm->fd,
			drm_conn->encoder_id);

		ssize_t index = -1;
		struct wlr_drm_connector *c, *wlr_conn = NULL;
		wl_list_for_each(c, &drm->outputs, link) {
			index++;
			if (c->id == drm_conn->connector_id) {
				wlr_conn = c;
				break;
			}
		}

		if (!wlr_conn) {
			wlr_conn = calloc(1, sizeof(*wlr_conn));
			if (!wlr_conn) {
				wlr_log_errno(WLR_ERROR, "Allocation failed");
				drmModeFreeEncoder(curr_enc);
				drmModeFreeConnector(drm_conn);
				continue;
			}
			wlr_output_init(&wlr_conn->output, &drm->backend, &output_impl,
				drm->display);

			struct wl_event_loop *ev = wl_display_get_event_loop(drm->display);
			wlr_conn->retry_pageflip = wl_event_loop_add_timer(ev, retry_pageflip,
				wlr_conn);

			wlr_conn->state = WLR_DRM_CONN_DISCONNECTED;
			wlr_conn->id = drm_conn->connector_id;

			snprintf(wlr_conn->output.name, sizeof(wlr_conn->output.name),
				"%s-%"PRIu32, conn_get_name(drm_conn->connector_type),
				drm_conn->connector_type_id);

			if (curr_enc) {
				wlr_conn->old_crtc = drmModeGetCrtc(drm->fd, curr_enc->crtc_id);
			}

			wl_list_insert(drm->outputs.prev, &wlr_conn->link);
			wlr_log(WLR_INFO, "Found connector '%s'", wlr_conn->output.name);
		} else {
			seen[index] = true;
		}

		if (curr_enc) {
			for (size_t i = 0; i < drm->num_crtcs; ++i) {
				if (drm->crtcs[i].id == curr_enc->crtc_id) {
					wlr_conn->crtc = &drm->crtcs[i];
					break;
				}
			}
		} else {
			wlr_conn->crtc = NULL;
		}

		// This can only happen *after* hotplug, since we haven't read the
		// connector properties yet
		if (wlr_conn->props.link_status != 0) {
			uint64_t link_status;
			if (!get_drm_prop(drm->fd, wlr_conn->id,
					wlr_conn->props.link_status, &link_status)) {
				wlr_log(WLR_ERROR, "Failed to get link status for '%s'",
					wlr_conn->output.name);
				continue;
			}

			if (link_status == DRM_MODE_LINK_STATUS_BAD) {
				// We need to reload our list of modes and force a modeset
				wlr_log(WLR_INFO, "Bad link for '%s'", wlr_conn->output.name);
				drm_connector_cleanup(wlr_conn);
			}
		}

		if (wlr_conn->state == WLR_DRM_CONN_DISCONNECTED &&
				drm_conn->connection == DRM_MODE_CONNECTED) {
			wlr_log(WLR_INFO, "'%s' connected", wlr_conn->output.name);
			wlr_log(WLR_DEBUG, "Current CRTC: %d",
				wlr_conn->crtc ? (int)wlr_conn->crtc->id : -1);

			wlr_conn->output.phys_width = drm_conn->mmWidth;
			wlr_conn->output.phys_height = drm_conn->mmHeight;
			wlr_log(WLR_INFO, "Physical size: %"PRId32"x%"PRId32,
				wlr_conn->output.phys_width, wlr_conn->output.phys_height);
			wlr_conn->output.subpixel = subpixel_map[drm_conn->subpixel];

			get_drm_connector_props(drm->fd, wlr_conn->id, &wlr_conn->props);

			size_t edid_len = 0;
			uint8_t *edid = get_drm_prop_blob(drm->fd,
				wlr_conn->id, wlr_conn->props.edid, &edid_len);
			parse_edid(&wlr_conn->output, edid_len, edid);
			free(edid);

			wlr_log(WLR_INFO, "Detected modes:");

			for (int i = 0; i < drm_conn->count_modes; ++i) {
				struct wlr_drm_mode *mode = calloc(1, sizeof(*mode));
				if (!mode) {
					wlr_log_errno(WLR_ERROR, "Allocation failed");
					continue;
				}

				if (drm_conn->modes[i].flags & DRM_MODE_FLAG_INTERLACE) {
					free(mode);
					continue;
				}

				mode->drm_mode = drm_conn->modes[i];
				mode->wlr_mode.width = mode->drm_mode.hdisplay;
				mode->wlr_mode.height = mode->drm_mode.vdisplay;
				mode->wlr_mode.refresh = calculate_refresh_rate(&mode->drm_mode);
				if (mode->drm_mode.type & DRM_MODE_TYPE_PREFERRED) {
					mode->wlr_mode.preferred = true;
				}

				wlr_log(WLR_INFO, "  %"PRId32"x%"PRId32"@%"PRId32,
					mode->wlr_mode.width, mode->wlr_mode.height,
					mode->wlr_mode.refresh);

				wl_list_insert(&wlr_conn->output.modes, &mode->wlr_mode.link);
			}

			wlr_conn->possible_crtc = get_possible_crtcs(drm->fd, res, drm_conn,
				wlr_conn->props.path != 0);
			if (wlr_conn->possible_crtc == 0) {
				wlr_log(WLR_ERROR, "No CRTC possible for connector '%s'",
					wlr_conn->output.name);
			}

			wlr_output_update_enabled(&wlr_conn->output, wlr_conn->crtc != NULL);
			wlr_conn->desired_enabled = true;

			wlr_conn->state = WLR_DRM_CONN_NEEDS_MODESET;
			new_outputs[new_outputs_len++] = wlr_conn;
		} else if ((wlr_conn->state == WLR_DRM_CONN_CONNECTED ||
				wlr_conn->state == WLR_DRM_CONN_NEEDS_MODESET) &&
				drm_conn->connection != DRM_MODE_CONNECTED) {
			wlr_log(WLR_INFO, "'%s' disconnected", wlr_conn->output.name);

			drm_connector_cleanup(wlr_conn);
		}

		drmModeFreeEncoder(curr_enc);
		drmModeFreeConnector(drm_conn);
	}

	drmModeFreeResources(res);

	// Iterate in reverse order because we'll remove items from the list and
	// still want indices to remain correct.
	struct wlr_drm_connector *conn, *tmp_conn;
	size_t index = wl_list_length(&drm->outputs);
	wl_list_for_each_reverse_safe(conn, tmp_conn, &drm->outputs, link) {
		index--;
		if (index >= seen_len || seen[index]) {
			continue;
		}

		wlr_log(WLR_INFO, "'%s' disappeared", conn->output.name);
		drm_connector_cleanup(conn);

		if (conn->pageflip_pending) {
			conn->state = WLR_DRM_CONN_DISAPPEARED;
		} else {
			wlr_output_destroy(&conn->output);
		}
	}

	realloc_crtcs(drm);

	for (size_t i = 0; i < new_outputs_len; ++i) {
		struct wlr_drm_connector *conn = new_outputs[i];

		wlr_log(WLR_INFO, "Requesting modeset for '%s'",
			conn->output.name);
		wlr_signal_emit_safe(&drm->backend.events.new_output,
			&conn->output);
	}

	attempt_enable_needs_modeset(drm);
}

static int mhz_to_nsec(int mhz) {
	return 1000000000000LL / mhz;
}

static void page_flip_handler(int fd, unsigned seq,
		unsigned tv_sec, unsigned tv_usec, unsigned crtc_id, void *data) {
	struct wlr_drm_backend *drm = data;
	struct wlr_drm_connector *conn = NULL;
	struct wlr_drm_connector *search;

	wl_list_for_each(search, &drm->outputs, link) {
		if (search->crtc && search->crtc->id == crtc_id) {
			conn = search;
		}
	}

	if (!conn) {
		wlr_log(WLR_ERROR, "No connector for crtc_id %u", crtc_id);
		return;
	}

	conn->pageflip_pending = false;

	if (conn->state == WLR_DRM_CONN_DISAPPEARED) {
		wlr_output_destroy(&conn->output);
		return;
	}

	if (conn->state != WLR_DRM_CONN_CONNECTED || conn->crtc == NULL) {
		return;
	}

	// Release the old buffer as it's not displayed anymore. The pending
	// buffer becomes the current buffer.
	wlr_buffer_unref(conn->current_buffer);
	conn->current_buffer = conn->pending_buffer;
	conn->pending_buffer = NULL;

	uint32_t present_flags = WLR_OUTPUT_PRESENT_VSYNC |
		WLR_OUTPUT_PRESENT_HW_CLOCK | WLR_OUTPUT_PRESENT_HW_COMPLETION;
	if (conn->current_buffer != NULL) {
		present_flags |= WLR_OUTPUT_PRESENT_ZERO_COPY;
	} else {
		post_drm_surface(&conn->crtc->primary->surf);
		if (drm->parent) {
			post_drm_surface(&conn->crtc->primary->mgpu_surf);
		}
	}

	struct timespec present_time = {
		.tv_sec = tv_sec,
		.tv_nsec = tv_usec * 1000,
	};
	struct wlr_output_event_present present_event = {
		.when = &present_time,
		.seq = seq,
		.refresh = mhz_to_nsec(conn->output.refresh),
		.flags = present_flags,
	};
	wlr_output_send_present(&conn->output, &present_event);

	if (drm->session->active) {
		wlr_output_send_frame(&conn->output);
	}
}

int handle_drm_event(int fd, uint32_t mask, void *data) {
	drmEventContext event = {
		.version = 3,
		.page_flip_handler2 = page_flip_handler,
	};

	drmHandleEvent(fd, &event);
	return 1;
}

void restore_drm_outputs(struct wlr_drm_backend *drm) {
	uint64_t to_close = (1L << wl_list_length(&drm->outputs)) - 1;

	struct wlr_drm_connector *conn;
	wl_list_for_each(conn, &drm->outputs, link) {
		if (conn->state == WLR_DRM_CONN_CONNECTED) {
			conn->state = WLR_DRM_CONN_CLEANUP;
		}
	}

	time_t timeout = time(NULL) + 5;

	while (to_close && time(NULL) < timeout) {
		handle_drm_event(drm->fd, 0, NULL);
		size_t i = 0;
		struct wlr_drm_connector *conn;
		wl_list_for_each(conn, &drm->outputs, link) {
			if (conn->state != WLR_DRM_CONN_CLEANUP || !conn->pageflip_pending) {
				to_close &= ~(1 << i);
			}
			i++;
		}
	}

	if (to_close) {
		wlr_log(WLR_ERROR, "Timed out stopping output renderers");
	}

	wl_list_for_each(conn, &drm->outputs, link) {
		drmModeCrtc *crtc = conn->old_crtc;
		if (!crtc) {
			continue;
		}

		drmModeSetCrtc(drm->fd, crtc->crtc_id, crtc->buffer_id, crtc->x, crtc->y,
			&conn->id, 1, &crtc->mode);
		drmModeSetCursor(drm->fd, crtc->crtc_id, 0, 0, 0);
	}
}

static void drm_connector_cleanup(struct wlr_drm_connector *conn) {
	if (!conn) {
		return;
	}

	switch (conn->state) {
	case WLR_DRM_CONN_CONNECTED:
	case WLR_DRM_CONN_CLEANUP:
		conn->output.current_mode = NULL;
		conn->desired_mode = NULL;
		struct wlr_drm_mode *mode, *tmp;
		wl_list_for_each_safe(mode, tmp, &conn->output.modes, wlr_mode.link) {
			wl_list_remove(&mode->wlr_mode.link);
			free(mode);
		}

		conn->output.enabled = false;
		conn->output.width = conn->output.height = conn->output.refresh = 0;

		memset(&conn->output.make, 0, sizeof(conn->output.make));
		memset(&conn->output.model, 0, sizeof(conn->output.model));
		memset(&conn->output.serial, 0, sizeof(conn->output.serial));

		if (conn->output.idle_frame != NULL) {
			wl_event_source_remove(conn->output.idle_frame);
			conn->output.idle_frame = NULL;
		}
		conn->output.needs_frame = false;
		conn->output.frame_pending = false;

		wlr_buffer_unref(conn->pending_buffer);
		wlr_buffer_unref(conn->current_buffer);
		conn->pending_buffer = conn->current_buffer = NULL;

		/* Fallthrough */
	case WLR_DRM_CONN_NEEDS_MODESET:
		wlr_log(WLR_INFO, "Emitting destruction signal for '%s'",
			conn->output.name);
		dealloc_crtc(conn);
		conn->possible_crtc = 0;
		conn->desired_mode = NULL;
		wlr_signal_emit_safe(&conn->output.events.destroy, &conn->output);
		break;
	case WLR_DRM_CONN_DISCONNECTED:
		break;
	case WLR_DRM_CONN_DISAPPEARED:
		return; // don't change state
	}

	conn->state = WLR_DRM_CONN_DISCONNECTED;
}
