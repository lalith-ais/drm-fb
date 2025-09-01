#include <drm_fourcc.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <signal.h>

#include "util.h"

struct dumb_framebuffer {
	uint32_t id;     // DRM object ID
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t handle; // driver-specific handle
	uint64_t size;   // size of mapping

	uint8_t *data;   // mmapped data we can write to
};

struct connector {
	uint32_t id;
	char name[16];
	bool connected;

	drmModeCrtc *saved;

	uint32_t crtc_id;
	drmModeModeInfo mode;

	uint32_t width;
	uint32_t height;
	uint32_t rate;

	struct dumb_framebuffer fb;

	struct connector *next;
};

// Global variable to track if we should exit
static volatile sig_atomic_t keep_running = 1;

// Signal handler for clean shutdown
void signal_handler(int sig)
{
	keep_running = 0;
}

static uint32_t find_crtc(int drm_fd, drmModeRes *res, drmModeConnector *conn,
		uint32_t *taken_crtcs)
{
	for (int i = 0; i < conn->count_encoders; ++i) {
		drmModeEncoder *enc = drmModeGetEncoder(drm_fd, conn->encoders[i]);
		if (!enc)
			continue;

		for (int i = 0; i < res->count_crtcs; ++i) {
			uint32_t bit = 1 << i;
			// Not compatible
			if ((enc->possible_crtcs & bit) == 0)
				continue;

			// Already taken
			if (*taken_crtcs & bit)
				continue;

			drmModeFreeEncoder(enc);
			*taken_crtcs |= bit;
			return res->crtcs[i];
		}

		drmModeFreeEncoder(enc);
	}

	return 0;
}

bool create_fb(int drm_fd, uint32_t width, uint32_t height, struct dumb_framebuffer *fb)
{
	int ret;

	struct drm_mode_create_dumb create = {
		.width = width,
		.height = height,
		.bpp = 32,
	};

	ret = drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
	if (ret < 0) {
		perror("DRM_IOCTL_MODE_CREATE_DUMB");
		return false;
	}

	fb->height = height;
	fb->width = width;
	fb->stride = create.pitch;
	fb->handle = create.handle;
	fb->size = create.size;

	uint32_t handles[4] = { fb->handle };
	uint32_t strides[4] = { fb->stride };
	uint32_t offsets[4] = { 0 };

	ret = drmModeAddFB2(drm_fd, width, height, DRM_FORMAT_XRGB8888,
			handles, strides, offsets, &fb->id, 0);
	if (ret < 0) {
		perror("drmModeAddFB2");
		goto error_dumb;
	}

	struct drm_mode_map_dumb map = { .handle = fb->handle };
	ret = drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
	if (ret < 0) {
		perror("DRM_IOCTL_MODE_MAP_DUMB");
		goto error_fb;
	}

	fb->data = mmap(0, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED,
			drm_fd, map.offset);
	if (!fb->data) {
		perror("mmap");
		goto error_fb;
	}

	memset(fb->data, 0xff, fb->size);

	return true;

error_fb:
	drmModeRmFB(drm_fd, fb->id);
error_dumb:
	;
	struct drm_mode_destroy_dumb destroy = { .handle = fb->handle };
	drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	return false;
}

bool load_splash_image_from_stdin(struct dumb_framebuffer *fb)
{
	printf("Reading splash image from stdin...\n");
	fflush(stdout);

	// Read the raw image data from stdin
	ssize_t total_read = 0;
	uint8_t *ptr = fb->data;
	size_t remaining = fb->size;

	while (remaining > 0) {
		ssize_t bytes_read = read(STDIN_FILENO, ptr, remaining);
		if (bytes_read < 0) {
			perror("read from stdin");
			return false;
		}
		if (bytes_read == 0) {
			// EOF reached
			break;
		}

		total_read += bytes_read;
		ptr += bytes_read;
		remaining -= bytes_read;
	}

	printf("Successfully read %zd bytes from stdin (expected %"PRIu64")\n", total_read, fb->size);
	fflush(stdout);

	if (total_read != fb->size) {
		fprintf(stderr, "Warning: Input image size (%zd bytes) doesn't match framebuffer size (%"PRIu64" bytes)\n", 
				total_read, fb->size);
	}

	return true;
}

void daemonize()
{
	pid_t pid = fork();

	if (pid < 0) {
		perror("fork");
		exit(1);
	}

	if (pid > 0) {
		// Parent process exits
		exit(0);
	}

	// Child process continues
	setsid(); // Create new session

	// Close standard file descriptors
	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);

	// Redirect standard file descriptors to /dev/null
	open("/dev/null", O_RDONLY); // stdin
	open("/dev/null", O_WRONLY); // stdout
	open("/dev/null", O_WRONLY); // stderr
}

int main(void)
{
	// Set up signal handlers
	signal(SIGTERM, signal_handler);
	signal(SIGINT, signal_handler);
	signal(SIGHUP, signal_handler);

	/* We just take the first GPU that exists. */
	int drm_fd = open("/dev/dri/card0", O_RDWR | O_NONBLOCK);
	if (drm_fd < 0) {
		perror("/dev/dri/card0");
		return 1;
	}

	drmModeRes *res = drmModeGetResources(drm_fd);
	if (!res) {
		perror("drmModeGetResources");
		return 1;
	}

	struct connector *conn_list = NULL;
	uint32_t taken_crtcs = 0;

	for (int i = 0; i < res->count_connectors; ++i) {
		drmModeConnector *drm_conn = drmModeGetConnector(drm_fd, res->connectors[i]);
		if (!drm_conn) {
			perror("drmModeGetConnector");
			continue;
		}

		struct connector *conn = malloc(sizeof *conn);
		if (!conn) {
			perror("malloc");
			goto cleanup;
		}

		conn->id = drm_conn->connector_id;
		snprintf(conn->name, sizeof conn->name, "%s-%"PRIu32,
				conn_str(drm_conn->connector_type),
				drm_conn->connector_type_id);
		conn->connected = drm_conn->connection == DRM_MODE_CONNECTED;

		conn->next = conn_list;
		conn_list = conn;

		printf("Found display %s\n", conn->name);
		fflush(stdout);

		if (!conn->connected) {
			printf("  Disconnected\n");
			fflush(stdout);
			goto cleanup;
		}

		if (drm_conn->count_modes == 0) {
			printf("No valid modes\n");
			fflush(stdout);
			conn->connected = false;
			goto cleanup;
		}

		conn->crtc_id = find_crtc(drm_fd, res, drm_conn, &taken_crtcs);
		if (!conn->crtc_id) {
			fprintf(stderr, "Could not find CRTC for %s\n", conn->name);
			conn->connected = false;
			goto cleanup;
		}

		printf("  Using CRTC %"PRIu32"\n", conn->crtc_id);
		fflush(stdout);

		// [0] is the best mode, so we'll just use that.
		conn->mode = drm_conn->modes[0];

		conn->width = conn->mode.hdisplay;
		conn->height = conn->mode.vdisplay;
		conn->rate = refresh_rate(&conn->mode);

		printf("  Using mode %"PRIu32"x%"PRIu32"@%"PRIu32"\n",
				conn->width, conn->height, conn->rate);
		fflush(stdout);

		if (!create_fb(drm_fd, conn->width, conn->height, &conn->fb)) {
			conn->connected = false;
			goto cleanup;
		}

		printf("  Created framebuffer with ID %"PRIu32" (size: %"PRIu64" bytes)\n", 
				conn->fb.id, conn->fb.size);
		fflush(stdout);

		// Load splash image from stdin into framebuffer
		if (!load_splash_image_from_stdin(&conn->fb)) {
			fprintf(stderr, "Failed to load splash image for %s\n", conn->name);
			conn->connected = false;
			goto cleanup;
		}

		// Save the previous CRTC configuration
		conn->saved = drmModeGetCrtc(drm_fd, conn->crtc_id);

		// Perform the modeset
		int ret = drmModeSetCrtc(drm_fd, conn->crtc_id, conn->fb.id, 0, 0,
				&conn->id, 1, &conn->mode);
		if (ret < 0) {
			perror("drmModeSetCrtc");
		}

cleanup:
		drmModeFreeConnector(drm_conn);
	}

	drmModeFreeResources(res);

	// Now daemonize after we've read from stdin
	printf("Daemonizing...\n");
	fflush(stdout);
	daemonize();

	// Keep running until we receive a signal
	while (keep_running) {
		sleep(1);
	}

	// Cleanup
	struct connector *conn = conn_list;
	while (conn) {
		if (conn->connected) {
			// Cleanup framebuffer
			munmap(conn->fb.data, conn->fb.size);
			drmModeRmFB(drm_fd, conn->fb.id);
			struct drm_mode_destroy_dumb destroy = { .handle = conn->fb.handle };
			drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);

			// Restore the old CRTC
			drmModeCrtc *crtc = conn->saved;
			if (crtc) {
				drmModeSetCrtc(drm_fd, crtc->crtc_id, crtc->buffer_id,
						crtc->x, crtc->y, &conn->id, 1, &crtc->mode);
				drmModeFreeCrtc(crtc);
			}
		}

		struct connector *tmp = conn->next;
		free(conn);
		conn = tmp;
	}

	close(drm_fd);
	return 0;
}
