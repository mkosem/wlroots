#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/dmabuf.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
#include "util/signal.h"
#include "wlr-export-dmabuf-unstable-v1-protocol.h"

#define EXPORT_DMABUF_MANAGER_VERSION 1


static const struct zwlr_export_dmabuf_frame_v1_interface frame_impl;

static struct wlr_export_dmabuf_frame_v1 *frame_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&zwlr_export_dmabuf_frame_v1_interface, &frame_impl));
	return wl_resource_get_user_data(resource);
}

static void frame_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zwlr_export_dmabuf_frame_v1_interface frame_impl = {
	.destroy = frame_handle_destroy,
};

static void frame_destroy(struct wlr_export_dmabuf_frame_v1 *frame) {
	if (frame == NULL) {
		return;
	}
	wlr_output_lock_attach_render(frame->output, false);
	if (frame->cursor_locked) {
		wlr_output_lock_software_cursors(frame->output, false);
	}
	wl_list_remove(&frame->link);
	wl_list_remove(&frame->output_precommit.link);
	wlr_dmabuf_attributes_finish(&frame->attribs);
	// Make the frame resource inert
	wl_resource_set_user_data(frame->resource, NULL);
	free(frame);
}

static void frame_handle_resource_destroy(struct wl_resource *resource) {
	struct wlr_export_dmabuf_frame_v1 *frame = frame_from_resource(resource);
	frame_destroy(frame);
}

static void frame_output_handle_precommit(struct wl_listener *listener,
		void *data) {
	struct wlr_export_dmabuf_frame_v1 *frame =
		wl_container_of(listener, frame, output_precommit);
	struct wlr_output_event_precommit *event = data;

	if (!(event->output->pending.committed & WLR_OUTPUT_STATE_BUFFER)) {
		return;
	}

	wl_list_remove(&frame->output_precommit.link);
	wl_list_init(&frame->output_precommit.link);

	time_t tv_sec = event->when->tv_sec;
	uint32_t tv_sec_hi = (sizeof(tv_sec) > 4) ? tv_sec >> 32 : 0;
	uint32_t tv_sec_lo = tv_sec & 0xFFFFFFFF;
	zwlr_export_dmabuf_frame_v1_send_ready(frame->resource,
		tv_sec_hi, tv_sec_lo, event->when->tv_nsec);
	frame_destroy(frame);
}


static const struct zwlr_export_dmabuf_manager_v1_interface manager_impl;

static struct wlr_export_dmabuf_manager_v1 *manager_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&zwlr_export_dmabuf_manager_v1_interface, &manager_impl));
	return wl_resource_get_user_data(resource);
}

static void manager_handle_capture_output(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id,
		int32_t overlay_cursor, struct wl_resource *output_resource) {
	struct wlr_export_dmabuf_manager_v1 *manager =
		manager_from_resource(manager_resource);
	struct wlr_output *output = wlr_output_from_resource(output_resource);

	struct wlr_export_dmabuf_frame_v1 *frame =
		calloc(1, sizeof(struct wlr_export_dmabuf_frame_v1));
	if (frame == NULL) {
		wl_resource_post_no_memory(manager_resource);
		return;
	}
	frame->manager = manager;
	frame->output = output;
	wl_list_init(&frame->output_precommit.link);

	uint32_t version = wl_resource_get_version(manager_resource);
	frame->resource = wl_resource_create(client,
		&zwlr_export_dmabuf_frame_v1_interface, version, id);
	if (frame->resource == NULL) {
		wl_client_post_no_memory(client);
		free(frame);
		return;
	}
	wl_resource_set_implementation(frame->resource, &frame_impl, frame,
		frame_handle_resource_destroy);

	wl_list_insert(&manager->frames, &frame->link);

	if (!output->impl->export_dmabuf) {
		zwlr_export_dmabuf_frame_v1_send_cancel(frame->resource,
			ZWLR_EXPORT_DMABUF_FRAME_V1_CANCEL_REASON_PERMANENT);
		frame_destroy(frame);
		return;
	}

	struct wlr_dmabuf_attributes *attribs = &frame->attribs;
	if (!wlr_output_export_dmabuf(output, attribs)) {
		zwlr_export_dmabuf_frame_v1_send_cancel(frame->resource,
			ZWLR_EXPORT_DMABUF_FRAME_V1_CANCEL_REASON_TEMPORARY);
		frame_destroy(frame);
		return;
	}

	wlr_output_lock_attach_render(frame->output, true);
	if (overlay_cursor) {
		wlr_output_lock_software_cursors(frame->output, true);
		frame->cursor_locked = true;
	}

	uint32_t frame_flags = ZWLR_EXPORT_DMABUF_FRAME_V1_FLAGS_TRANSIENT;
	uint32_t mod_high = attribs->modifier >> 32;
	uint32_t mod_low = attribs->modifier & 0xFFFFFFFF;

	zwlr_export_dmabuf_frame_v1_send_frame(frame->resource,
		output->width, output->height, 0, 0, attribs->flags, frame_flags,
		attribs->format, mod_high, mod_low, attribs->n_planes);

	for (int i = 0; i < attribs->n_planes; ++i) {
		off_t size = lseek(attribs->fd[i], 0, SEEK_END);

		zwlr_export_dmabuf_frame_v1_send_object(frame->resource, i,
			attribs->fd[i], size, attribs->offset[i], attribs->stride[i], i);
	}

	wl_list_remove(&frame->output_precommit.link);
	wl_signal_add(&output->events.precommit, &frame->output_precommit);
	frame->output_precommit.notify = frame_output_handle_precommit;
}

static void manager_handle_destroy(struct wl_client *client,
		struct wl_resource *manager_resource) {
	wl_resource_destroy(manager_resource);
}

static const struct zwlr_export_dmabuf_manager_v1_interface manager_impl = {
	.capture_output = manager_handle_capture_output,
	.destroy = manager_handle_destroy,
};

static void manager_handle_resource_destroy(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

static void manager_bind(struct wl_client *client, void *data, uint32_t version,
		uint32_t id) {
	struct wlr_export_dmabuf_manager_v1 *manager = data;

	struct wl_resource *resource = wl_resource_create(client,
		&zwlr_export_dmabuf_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, manager,
		manager_handle_resource_destroy);

	wl_list_insert(&manager->resources, wl_resource_get_link(resource));
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_export_dmabuf_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);
	wlr_export_dmabuf_manager_v1_destroy(manager);
}

struct wlr_export_dmabuf_manager_v1 *wlr_export_dmabuf_manager_v1_create(
		struct wl_display *display) {
	struct wlr_export_dmabuf_manager_v1 *manager =
		calloc(1, sizeof(struct wlr_export_dmabuf_manager_v1));
	if (manager == NULL) {
		return NULL;
	}
	wl_list_init(&manager->resources);
	wl_list_init(&manager->frames);
	wl_signal_init(&manager->events.destroy);

	manager->global = wl_global_create(display,
		&zwlr_export_dmabuf_manager_v1_interface, EXPORT_DMABUF_MANAGER_VERSION,
		manager, manager_bind);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;
}

void wlr_export_dmabuf_manager_v1_destroy(
		struct wlr_export_dmabuf_manager_v1 *manager) {
	if (manager == NULL) {
		return;
	}
	wlr_signal_emit_safe(&manager->events.destroy, manager);
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	struct wl_resource *resource, *resource_tmp;
	wl_resource_for_each_safe(resource, resource_tmp, &manager->resources) {
		wl_resource_destroy(resource);
	}
	struct wlr_export_dmabuf_frame_v1 *frame, *frame_tmp;
	wl_list_for_each_safe(frame, frame_tmp, &manager->frames, link) {
		wl_resource_destroy(frame->resource);
	}
	free(manager);
}
