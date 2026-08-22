/* hd60s-dumpram -- read device memory over the Cypress FX3 vendor RAM command.
 *
 * The HD60 S ships with bRequest 0xA0 (the standard Cypress RAM access command)
 * still enabled. Address is split across the setup packet the usual FX3 way:
 *   wValue = addr >> 16, wIndex = addr & 0xffff
 *
 * SAFETY: reads only. Every transfer is bmRequestType 0xC0 -- vendor, IN,
 * device recipient. The matching OUT direction (0x40) is how you *write* RAM
 * and is how firmware gets loaded; this tool must never issue it. Writing the
 * wrong thing here bricks the device.
 *
 * Two different failure modes, and conflating them is dangerous:
 *   STALL (LIBUSB_ERROR_PIPE) -- nothing mapped at that address. Harmless, the
 *     device is fine, record a gap and carry on.
 *   TIMEOUT -- the device did not answer at all. EP0 is now wedged, and every
 *     further transfer makes it worse. Observed on real hardware: continuing
 *     past a timeout took the device off the bus entirely and needed a physical
 *     replug. So we stop dead on the first timeout.
 */
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libusb-1.0/libusb.h>

#include "../src/hd60s.h"

#define VENDOR_RAM 0xA0
#define RQ_IN (LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN | LIBUSB_RECIPIENT_DEVICE)

/* FX3 SYSMEM. 512 KB on the CYUSB3014; the running firmware lives here. */
#define FX3_SYSMEM 0x40000000u
#define FX3_SYSMEM_LEN 0x80000u

static volatile sig_atomic_t stop;

static void on_sig(int s)
{
	(void)s;
	stop = 1;
}

static int read_chunk(libusb_device_handle *h, uint32_t addr, uint8_t *buf,
		      int len)
{
	return libusb_control_transfer(h, RQ_IN, VENDOR_RAM,
				       (uint16_t)(addr >> 16),
				       (uint16_t)(addr & 0xffff), buf,
				       (uint16_t)len, 1000);
}

int main(int argc, char **argv)
{
	libusb_context *ctx = NULL;
	libusb_device_handle *h;
	uint32_t addr = FX3_SYSMEM, len = FX3_SYSMEM_LEN;
	int chunk = 64, i, verify = 0;
	const char *out = NULL;
	FILE *f = NULL;
	uint8_t *buf, *vbuf;
	uint32_t done = 0, gaps = 0;
	int delay_ms = 5, timed_out = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--addr") && i + 1 < argc)
			addr = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--len") && i + 1 < argc)
			len = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--chunk") && i + 1 < argc)
			chunk = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--out") && i + 1 < argc)
			out = argv[++i];
		else if (!strcmp(argv[i], "--verify"))
			verify = 1;
		else if (!strcmp(argv[i], "--delay") && i + 1 < argc)
			delay_ms = atoi(argv[++i]);
		else {
			fprintf(stderr,
				"usage: %s [--addr A] [--len N] [--chunk N] [--out FILE] [--verify]\n"
				"  defaults to the FX3 SYSMEM window (0x%08x, %u bytes)\n"
				"  --verify    re-read each chunk and flag mismatches\n"
				"  --delay N   ms to pause between reads (default 5)\n"
				"  with no --out, hexdumps to stdout\n",
				argv[0], FX3_SYSMEM, FX3_SYSMEM_LEN);
			return 2;
		}
	}

	if (chunk < 1 || chunk > 4096) {
		log_err("chunk must be 1..4096");
		return 2;
	}

	buf = malloc(chunk);
	vbuf = malloc(chunk);
	if (!buf || !vbuf) {
		log_err("out of memory");
		return 1;
	}

	signal(SIGINT, on_sig);
	if (libusb_init(&ctx) != 0) {
		log_err("libusb_init failed");
		return 1;
	}
	h = libusb_open_device_with_vid_pid(ctx, HD60S_VID, HD60S_PID);
	if (!h) {
		log_err("no %04x:%04x, or permission denied", HD60S_VID, HD60S_PID);
		libusb_exit(ctx);
		return 1;
	}

	if (out) {
		f = fopen(out, "wb");
		if (!f) {
			log_err("cannot open %s: %s", out, strerror(errno));
			libusb_close(h);
			libusb_exit(ctx);
			return 1;
		}
	}

	log_info("reading 0x%08x..0x%08x (%u bytes) in %d-byte chunks", addr,
		 addr + len, len, chunk);

	while (done < len && !stop) {
		int want = (int)(len - done);
		uint32_t a = addr + done;
		int rc;

		if (want > chunk)
			want = chunk;

		rc = read_chunk(h, a, buf, want);
		if (rc == LIBUSB_ERROR_TIMEOUT) {
			/* EP0 is wedged. Hammering it from here takes the device
			 * off the bus completely -- stop and let the user replug. */
			log_err("timeout at 0x%08x -- device stopped answering.", a);
			log_err("stopping now; further transfers only make it worse.");
			log_err("unplug and replug the device before retrying.");
			timed_out = 1;
			break;
		}
		if (rc < 0) {
			/* stall: nothing mapped here. Pad so file offsets stay true. */
			if (f) {
				memset(buf, 0xff, want);
				fwrite(buf, 1, want, f);
			} else {
				printf("%08x  <unmapped: %s>\n", a,
				       libusb_error_name(rc));
			}
			gaps++;
			done += want;
			continue;
		}

		if (delay_ms)
			usleep(delay_ms * 1000);

		if (verify) {
			int rc2 = read_chunk(h, a, vbuf, want);

			if (rc2 == rc && memcmp(buf, vbuf, rc) != 0)
				log_warn("0x%08x differs between reads "
					 "(live register or DMA buffer?)", a);
		}

		if (f)
			fwrite(buf, 1, rc, f);
		else
			hd60s_hexdump(stdout, "", buf, rc);

		done += rc;
		if (f && (done % 0x10000) == 0)
			log_info("  %u / %u bytes", done, len);
		if (rc < want)
			break; /* short read: end of the mapped window */
	}

	if (f) {
		fclose(f);
		log_info("wrote %u bytes to %s", done, out);
	}
	if (gaps)
		log_info("%u chunk(s) returned no data (unmapped)", gaps);

	libusb_close(h);
	libusb_exit(ctx);
	free(buf);
	free(vbuf);
	return timed_out ? 1 : 0;
}
