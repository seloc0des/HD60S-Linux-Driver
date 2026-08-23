/* Frame sinks: raw stream, and v4l2loopback for real applications.
 *
 * v4l2loopback is the pragmatic target -- OBS, ffmpeg, browsers and anything
 * else just see a camera. It costs one memcpy per frame versus a native kernel
 * driver, which is nothing next to not having a driver at all.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/videodev2.h>

#include "frame.h"
#include "hd60s.h"

struct fd_sink {
	int fd;
	int own_fd; /* don't close stdout */
	char path[256];
};

static int write_all(int fd, const uint8_t *p, size_t n)
{
	while (n) {
		ssize_t k = write(fd, p, n);

		if (k < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		p += k;
		n -= (size_t)k;
	}
	return 0;
}

/* --- raw sink ---------------------------------------------------------- */

static int raw_open(struct hd60s_sink *s, uint32_t w, uint32_t h, uint32_t fourcc)
{
	struct fd_sink *p = s->priv;

	if (p->fd >= 0)
		return 0; /* stdout, already open */
	p->fd = open(p->path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (p->fd < 0) {
		log_err("open %s: %s", p->path, strerror(errno));
		return -1;
	}
	p->own_fd = 1;
	log_info("raw sink: %s (%ux%u, %.4s)", p->path, w, h, (char *)&fourcc);
	return 0;
}

static int raw_write(struct hd60s_sink *s, const struct hd60s_frame *f)
{
	struct fd_sink *p = s->priv;
	int rc = write_all(p->fd, f->data, f->size);

	if (rc < 0)
		log_err("write: %s", strerror(-rc));
	return rc;
}

static void raw_close(struct hd60s_sink *s)
{
	struct fd_sink *p = s->priv;

	if (p->own_fd && p->fd >= 0)
		close(p->fd);
	free(p);
	free(s);
}

/* --- v4l2loopback sink ------------------------------------------------- */

static int v4l2_open(struct hd60s_sink *s, uint32_t w, uint32_t h, uint32_t fourcc)
{
	struct fd_sink *p = s->priv;
	struct v4l2_format fmt;

	p->fd = open(p->path, O_WRONLY);
	if (p->fd < 0) {
		log_err("open %s: %s", p->path, strerror(errno));
		log_err("is v4l2loopback loaded?  sudo modprobe v4l2loopback");
		return -1;
	}
	p->own_fd = 1;

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width = w;
	fmt.fmt.pix.height = h;
	fmt.fmt.pix.pixelformat = fourcc;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	fmt.fmt.pix.bytesperline = w * 2;
	fmt.fmt.pix.sizeimage = (uint32_t)hd60s_frame_size(w, h, fourcc);
	fmt.fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;

	if (ioctl(p->fd, VIDIOC_S_FMT, &fmt) < 0) {
		log_err("VIDIOC_S_FMT on %s: %s", p->path, strerror(errno));
		close(p->fd);
		p->fd = -1;
		return -1;
	}
	log_info("v4l2 sink: %s (%ux%u, %.4s)", p->path, w, h, (char *)&fourcc);
	return 0;
}

/* --- factory ----------------------------------------------------------- */

struct hd60s_sink *hd60s_sink_create(const char *spec)
{
	struct hd60s_sink *s = calloc(1, sizeof(*s));
	struct fd_sink *p = calloc(1, sizeof(*p));

	if (!s || !p) {
		free(s);
		free(p);
		return NULL;
	}
	p->fd = -1;

	if (!strcmp(spec, "-") || !strcmp(spec, "stdout")) {
		p->fd = STDOUT_FILENO;
		snprintf(p->path, sizeof(p->path), "<stdout>");
		s->name = "raw";
		s->open = raw_open;
	} else if (!strncmp(spec, "raw:", 4)) {
		snprintf(p->path, sizeof(p->path), "%s", spec + 4);
		s->name = "raw";
		s->open = raw_open;
	} else if (!strncmp(spec, "v4l2:", 5)) {
		snprintf(p->path, sizeof(p->path), "%s", spec + 5);
		s->name = "v4l2";
		s->open = v4l2_open;
	} else {
		log_err("unknown sink '%s' (want raw:PATH, v4l2:/dev/videoN, or -)",
			spec);
		free(s);
		free(p);
		return NULL;
	}

	s->write = raw_write; /* v4l2loopback takes plain write() too */
	s->close = raw_close;
	s->priv = p;
	return s;
}
