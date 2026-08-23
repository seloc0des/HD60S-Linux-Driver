/* hd60s-isotest -- try to pull data off the isochronous streaming endpoint.
 *
 * Strictly read-only: selects an alternate setting on interface 0 and submits
 * IN transfers. No vendor command is sent, nothing is written.
 *
 * The question it answers: does the device put anything on EP 0x83 without
 * being told to? If yes, capture may be far simpler than assumed. If every
 * packet comes back zero-length, the stream needs arming by a vendor command
 * we don't know yet -- which is itself worth documenting.
 *
 * Note alt 0 has wMaxPacketSize 0, i.e. it reserves no bus bandwidth: that is
 * the idle setting. Alt 1 is 1024 B/uframe and alt 2 is 3072 B/uframe.
 */
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libusb-1.0/libusb.h>

#include "../src/hd60s.h"

#define NUM_TRANSFERS 8
#define PKTS_PER_XFER 64

static volatile sig_atomic_t stop;
static unsigned long long total_bytes, total_pkts, nonempty_pkts, err_pkts;
static int dumped;

static void on_sig(int s)
{
	(void)s;
	stop = 1;
}

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void LIBUSB_CALL cb(struct libusb_transfer *xfer)
{
	int i;

	if (xfer->status != LIBUSB_TRANSFER_COMPLETED &&
	    xfer->status != LIBUSB_TRANSFER_TIMED_OUT) {
		if (xfer->status == LIBUSB_TRANSFER_CANCELLED ||
		    xfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
			free(xfer->buffer);
			libusb_free_transfer(xfer);
			return;
		}
		log_warn("transfer status %d", xfer->status);
	}

	for (i = 0; i < xfer->num_iso_packets; i++) {
		struct libusb_iso_packet_descriptor *d = &xfer->iso_packet_desc[i];

		total_pkts++;
		if (d->status != LIBUSB_TRANSFER_COMPLETED) {
			err_pkts++;
			continue;
		}
		if (!d->actual_length)
			continue;

		nonempty_pkts++;
		total_bytes += d->actual_length;

		/* show the first few non-empty packets -- that's the framing */
		if (dumped < 4) {
			uint8_t *p = libusb_get_iso_packet_buffer_simple(xfer, i);
			int n = d->actual_length > 64 ? 64 : (int)d->actual_length;

			printf("\niso packet: %u byte(s)\n", d->actual_length);
			hd60s_hexdump(stdout, "    ", p, n);
			dumped++;
		}
	}

	if (!stop && libusb_submit_transfer(xfer) != 0) {
		free(xfer->buffer);
		libusb_free_transfer(xfer);
	}
}

int main(int argc, char **argv)
{
	libusb_context *ctx = NULL;
	libusb_device_handle *h;
	struct libusb_transfer *xfers[NUM_TRANSFERS];
	int alt = 2, seconds = 5, pkt_size = 3072, i, rc, live = 0;
	double t0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--alt") && i + 1 < argc)
			alt = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
			seconds = atoi(argv[++i]);
		else {
			fprintf(stderr,
				"usage: %s [--alt N] [--seconds N]\n"
				"  --alt      interface 0 alt setting: 1 (1024 B) or 2 (3072 B)\n"
				"             alt 0 reserves no bandwidth and cannot stream\n"
				"  --seconds  how long to listen (default 5)\n",
				argv[0]);
			return 2;
		}
	}
	if (alt == 1)
		pkt_size = 1024;
	else if (alt != 2) {
		log_err("alt must be 1 or 2 (alt 0 is the idle setting, 0 bytes)");
		return 2;
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

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
	libusb_set_auto_detach_kernel_driver(h, 1);

	rc = libusb_claim_interface(h, HD60S_IF_STREAM);
	if (rc != 0) {
		log_err("claim interface %d: %s", HD60S_IF_STREAM,
			libusb_error_name(rc));
		goto out;
	}
	rc = libusb_set_interface_alt_setting(h, HD60S_IF_STREAM, alt);
	if (rc != 0) {
		log_err("set alt %d: %s", alt, libusb_error_name(rc));
		log_err("the host may not have the iso bandwidth free");
		goto out_release;
	}
	log_info("interface %d alt %d selected, %d B/microframe on ep 0x%02x",
		 HD60S_IF_STREAM, alt, pkt_size, HD60S_EP_STREAM_ISO);

	for (i = 0; i < NUM_TRANSFERS; i++) {
		size_t len = (size_t)pkt_size * PKTS_PER_XFER;
		uint8_t *buf = malloc(len);

		xfers[i] = libusb_alloc_transfer(PKTS_PER_XFER);
		if (!buf || !xfers[i]) {
			log_err("out of memory");
			free(buf);
			goto out_release;
		}
		libusb_fill_iso_transfer(xfers[i], h, HD60S_EP_STREAM_ISO, buf,
					 (int)len, PKTS_PER_XFER, cb, NULL, 1000);
		libusb_set_iso_packet_lengths(xfers[i], pkt_size);

		rc = libusb_submit_transfer(xfers[i]);
		if (rc != 0) {
			log_warn("submit %d: %s", i, libusb_error_name(rc));
			free(buf);
			libusb_free_transfer(xfers[i]);
			xfers[i] = NULL;
			continue;
		}
		live++;
	}
	if (!live) {
		log_err("no transfers could be submitted");
		goto out_release;
	}
	log_info("listening %ds with %d transfers of %d packets...", seconds,
		 live, PKTS_PER_XFER);

	t0 = now_s();
	while (!stop && now_s() - t0 < seconds) {
		struct timeval tv = { 0, 100000 };

		if (libusb_handle_events_timeout(ctx, &tv) != 0)
			break;
	}
	stop = 1;

	for (i = 0; i < NUM_TRANSFERS; i++)
		if (xfers[i])
			libusb_cancel_transfer(xfers[i]);
	for (i = 0; i < 20; i++) {
		struct timeval tv = { 0, 50000 };

		libusb_handle_events_timeout(ctx, &tv);
	}

	printf("\n--- results over %.1fs ---\n", now_s() - t0);
	printf("  iso packets serviced : %llu\n", total_pkts);
	printf("  packets with data    : %llu\n", nonempty_pkts);
	printf("  packets errored      : %llu\n", err_pkts);
	printf("  total bytes          : %llu\n", total_bytes);
	if (!nonempty_pkts)
		printf("\n  Every packet came back empty. The endpoint is serviced but the\n"
		       "  device sends nothing, so the stream needs arming by a vendor\n"
		       "  command we do not know yet.\n");
	else
		printf("\n  Data is flowing without any vendor command!\n");

out_release:
	libusb_set_interface_alt_setting(h, HD60S_IF_STREAM, 0); /* back to idle */
	libusb_release_interface(h, HD60S_IF_STREAM);
out:
	libusb_close(h);
	libusb_exit(ctx);
	return 0;
}
