/*
 * Copyright (c) 2017 Rob Clark <rclark@redhat.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/event.h>

#include "common.h"
#include "drm-common.h"

static struct drm drm;

static void page_flip_handler(int fd, unsigned int frame,
		  unsigned int sec, unsigned int usec, void *data)
{
	/* suppress 'unused parameter' warnings */
	(void)fd, (void)frame, (void)sec, (void)usec;

	int *waiting_for_flip = data;
	*waiting_for_flip = 0;
}

static int kqueue_run(const struct gbm *gbm, const struct egl *egl)
{
	fd_set fds;
	drmEventContext evctx = {
			.version = 2,
			.page_flip_handler = page_flip_handler,
	};
	struct gbm_bo *bo;
	struct drm_fb *fb;
	struct kevent ke;
	uint32_t i = 0;
	int ret, kq;

	kq = kqueue();
	if (kq == -1) {
		fprintf(stderr, "Failed to create kqueue\n");
		return -1;
	}
	memset(&ke, 0, sizeof(struct kevent));
	EV_SET(&ke, drm.fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 5, NULL);

	if (kevent(kq, &ke, 1, NULL, 0, NULL) == -1) {
		fprintf(stderr, "Failed to create kevent\n");
		return -1;
	}

	FD_ZERO(&fds);
	FD_SET(0, &fds);
	FD_SET(drm.fd, &fds);

	eglSwapBuffers(egl->display, egl->surface);
	bo = gbm_surface_lock_front_buffer(gbm->surface);
	fb = drm_fb_get_from_bo(bo);
	if (!fb) {
		fprintf(stderr, "Failed to get a new framebuffer BO\n");
		return -1;
	}

	/* set mode: */
	ret = drmModeSetCrtc(drm.fd, drm.crtc_id, fb->fb_id, 0, 0,
			&drm.connector_id, 1, drm.mode);
	if (ret) {
		printf("failed to set mode: %s\n", strerror(errno));
		return ret;
	}

	while (1) {
		struct gbm_bo *next_bo;
		int waiting_for_flip = 1;

		egl->draw(i++);

		eglSwapBuffers(egl->display, egl->surface);
		next_bo = gbm_surface_lock_front_buffer(gbm->surface);
		fb = drm_fb_get_from_bo(next_bo);
		if (!fb) {
			fprintf(stderr, "Failed to get a new framebuffer BO\n");
			return -1;
		}

		/*
		 * Here you could also update drm plane layers if you want
		 * hw composition
		 */

		ret = drmModePageFlip(drm.fd, drm.crtc_id, fb->fb_id,
				DRM_MODE_PAGE_FLIP_EVENT, &waiting_for_flip);
		if (ret) {
			printf("failed to queue page flip: %s\n", strerror(errno));
			return -1;
		}

		while (waiting_for_flip) {
			memset(&ke, 0, sizeof(ke));
			/* receive an event, a blocking call as timeout is NULL */
			ret = kevent(kq, NULL, 0, &ke, 1, NULL);
			if (ret == -1) {
				fprintf(stderr, "kevent() error\n");
				return -1;
			} else if (ret == 0)
				fprintf(stderr, "kevent() timeout...\n");

			if (ke.ident != (unsigned int)drm.fd) {
				fprintf(stderr, "ke.ident mismatch\n");
				return -1;
			}
			drmHandleEvent(drm.fd, &evctx);
		}

		/* release last buffer to render on again: */
		gbm_surface_release_buffer(gbm->surface, bo);
		bo = next_bo;
	}

	return 0;
}

const struct drm * init_drm_kqueue(const char *device)
{
	int ret;

	ret = init_drm(&drm, device);
	if (ret)
		return NULL;

	drm.run = kqueue_run;

	return &drm;
}
