#include <assert.h>
#include <xf86drm.h>
#include <stdlib.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/render/timeline.h>
#include <wlr/util/log.h>

#include "config.h"

#if HAVE_EVENTFD
#include <sys/eventfd.h>
#endif

struct wlr_render_timeline {
	int drm_fd;
	uint32_t handle;
};

struct wlr_render_timeline *wlr_render_timeline_create(int drm_fd) {
	struct wlr_render_timeline *timeline = calloc(1, sizeof(*timeline));
	if (timeline == NULL) {
		return NULL;
	}
	timeline->drm_fd = drm_fd;

	if (drmSyncobjCreate(drm_fd, 0, &timeline->handle) != 0) {
		wlr_log_errno(WLR_ERROR, "drmSyncobjCreate failed");
		free(timeline);
		return NULL;
	}

	return timeline;
}

struct wlr_render_timeline *wlr_render_timeline_import(int drm_fd,
		int drm_syncobj_fd) {
	struct wlr_render_timeline *timeline = calloc(1, sizeof(*timeline));
	if (timeline == NULL) {
		return NULL;
	}
	timeline->drm_fd = drm_fd;

	if (drmSyncobjFDToHandle(drm_fd, drm_syncobj_fd, &timeline->handle) != 0) {
		wlr_log_errno(WLR_ERROR, "drmSyncobjFDToHandle failed");
		free(timeline);
		return NULL;
	}

	return timeline;
}

void wlr_render_timeline_destroy(struct wlr_render_timeline *timeline) {
	drmSyncobjDestroy(timeline->drm_fd, timeline->handle);
	free(timeline);
}

bool wlr_render_timeline_transfer(struct wlr_render_timeline *dst,
		uint64_t dst_point, struct wlr_render_timeline *src, uint64_t src_point) {
	assert(dst->drm_fd == src->drm_fd);

	if (drmSyncobjTransfer(dst->drm_fd, dst->handle, dst_point,
			src->handle, src_point, 0) != 0) {
		wlr_log_errno(WLR_ERROR, "drmSyncobjTransfer failed");
		return false;
	}

	return true;
}

int wlr_render_timeline_export_sync_file(struct wlr_render_timeline *timeline,
		uint64_t src_point) {
	int sync_file_fd = -1;

	uint32_t syncobj_handle;
	if (drmSyncobjCreate(timeline->drm_fd, 0, &syncobj_handle) != 0) {
		wlr_log_errno(WLR_ERROR, "drmSyncobjCreate failed");
		return -1;
	}

	if (drmSyncobjTransfer(timeline->drm_fd, syncobj_handle, 0,
			timeline->handle, src_point, 0) != 0) {
		wlr_log_errno(WLR_ERROR, "drmSyncobjTransfer failed");
		goto out;
	}

	if (drmSyncobjExportSyncFile(timeline->drm_fd,
			syncobj_handle, &sync_file_fd) != 0) {
		wlr_log_errno(WLR_ERROR, "drmSyncobjExportSyncFile failed");
		goto out;
	}

out:
	drmSyncobjDestroy(timeline->drm_fd, syncobj_handle);
	return sync_file_fd;
}

bool wlr_render_timeline_import_sync_file(struct wlr_render_timeline *timeline,
		uint64_t dst_point, int sync_file_fd) {
	bool ok = false;

	uint32_t syncobj_handle;
	if (drmSyncobjCreate(timeline->drm_fd, 0, &syncobj_handle) != 0) {
		wlr_log_errno(WLR_ERROR, "drmSyncobjCreate failed");
		return -1;
	}

	if (drmSyncobjImportSyncFile(timeline->drm_fd, syncobj_handle,
			sync_file_fd) != 0) {
		wlr_log_errno(WLR_ERROR, "drmSyncobjImportSyncFile failed");
		goto out;
	}

	if (drmSyncobjTransfer(timeline->drm_fd, timeline->handle, dst_point,
			syncobj_handle, 0, 0) != 0) {
		wlr_log_errno(WLR_ERROR, "drmSyncobjTransfer failed");
		goto out;
	}

	ok = true;

out:
	drmSyncobjDestroy(timeline->drm_fd, syncobj_handle);
	return ok;
}

#ifndef DRM_IOCTL_SYNCOBJ_EVENTFD

struct drm_syncobj_eventfd {
	__u32 handle;
	__u32 flags;
	__u64 point;
	__s32 fd;
	__u32 pad;
};

#define DRM_IOCTL_SYNCOBJ_EVENTFD DRM_IOWR(0xCE, struct drm_syncobj_eventfd)

#endif

struct wlr_render_timeline_waiter {
	wlr_render_timeline_wait_func_t func;
	void *data;
};

static int handle_eventfd_ready(int ev_fd, uint32_t mask, void *data) {
	struct wlr_render_timeline_waiter *waiter = data;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		wlr_log(WLR_ERROR, "Failed to wait for render timeline: eventfd error");
	}

	if (mask & WL_EVENT_READABLE) {
		uint64_t ev_fd_value;
		if (read(ev_fd, &ev_fd_value, sizeof(ev_fd_value)) <= 0) {
			wlr_log(WLR_ERROR, "Failed to wait for render timeline: read() failed");
		}
	}

	close(ev_fd);
	waiter->func(waiter->data);
	free(waiter);
	return 0;
}

struct wl_event_source *wlr_render_timeline_wait(struct wlr_render_timeline *timeline,
		uint64_t point, uint32_t flags, struct wl_event_loop *loop,
		wlr_render_timeline_wait_func_t func, void *data) {
	uint32_t signaled_point;
	int ret = drmSyncobjTimelineWait(timeline->drm_fd, &timeline->handle, &point, 1, 0, flags, &signaled_point);
	if (ret == 0) {
		func(data);
		return NULL;
	} else if (ret != -ETIME) {
		wlr_log_errno(WLR_ERROR, "drmSyncobjWait() failed");
		return NULL;
	}

	int ev_fd;
#if HAVE_EVENTFD
	ev_fd = eventfd(0, EFD_CLOEXEC);
	if (ev_fd < 0) {
		wlr_log_errno(WLR_ERROR, "eventfd() failed");
	}
#else
	ev_fd = -1;
	wlr_log(WLR_ERROR, "eventfd() is unavailable");
#endif
	if (ev_fd < 0) {
		return NULL;
	}

	struct drm_syncobj_eventfd syncobj_eventfd = {
		.handle = timeline->handle,
		.flags = flags,
		.point = point,
		.fd = ev_fd,
	};
	if (drmIoctl(timeline->drm_fd, DRM_IOCTL_SYNCOBJ_EVENTFD, &syncobj_eventfd) != 0) {
		wlr_log_errno(WLR_ERROR, "DRM_IOCTL_SYNCOBJ_EVENTFD failed");
		close(ev_fd);
		return NULL;
	}

	struct wlr_render_timeline_waiter *waiter = calloc(1, sizeof(*waiter));
	if (waiter == NULL) {
		wlr_log(WLR_ERROR, "Allocation failed");
		close(ev_fd);
		return NULL;
	}

	waiter->func = func;
	waiter->data = data;

	return wl_event_loop_add_fd(loop, ev_fd, WL_EVENT_READABLE, handle_eventfd_ready, waiter);
}
