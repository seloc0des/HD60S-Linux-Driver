/* hd60s-probe -- read-only reverse-engineering probe for the Elgato HD60 S.
 *
 * SAFETY: this tool never issues a host-to-device vendor request. Every EP0
 * transfer it sends is device-to-host (bmRequestType bit 7 set). The HD60 S
 * boots from unprotected SPI flash, so a stray vendor write can brick it.
 * Keep it that way -- if you add a write path, gate it behind its own flag and
 * say so loudly.
 *
 * What it does:
 *   - opens 0fd9:005e, reports negotiated speed and the active descriptor set
 *   - listens on the interrupt endpoint (0x81) and dumps whatever the device
 *     volunteers, with arrival timestamps
 *   - optionally sweeps EP0 vendor IN requests to find which bRequest values
 *     are implemented (they return data) vs unimplemented (they stall)
 */
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libusb-1.0/libusb.h>

#include "../src/hd60s.h"

static volatile sig_atomic_t stop;

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

static const char *speed_str(int s)
{
	switch (s) {
	case LIBUSB_SPEED_LOW: return "Low (1.5M)";
	case LIBUSB_SPEED_FULL: return "Full (12M)";
	case LIBUSB_SPEED_HIGH: return "High (480M)";
	case LIBUSB_SPEED_SUPER: return "SuperSpeed (5G)";
	case LIBUSB_SPEED_SUPER_PLUS: return "SuperSpeed+ (10G)";
	default: return "unknown";
	}
}

static void dump_active_config(libusb_device *dev)
{
	struct libusb_config_descriptor *cfg;
	int i, a, e;

	if (libusb_get_active_config_descriptor(dev, &cfg) != 0) {
		log_warn("cannot read active config descriptor");
		return;
	}
	log_info("active config: %d interface(s), %d mA",
		 cfg->bNumInterfaces, cfg->MaxPower * 2);

	for (i = 0; i < cfg->bNumInterfaces; i++) {
		const struct libusb_interface *itf = &cfg->interface[i];

		for (a = 0; a < itf->num_altsetting; a++) {
			const struct libusb_interface_descriptor *id =
				&itf->altsetting[a];

			printf("  if%d alt%d  class=0x%02x  %d endpoint(s)\n",
			       id->bInterfaceNumber, id->bAlternateSetting,
			       id->bInterfaceClass, id->bNumEndpoints);

			for (e = 0; e < id->bNumEndpoints; e++) {
				const struct libusb_endpoint_descriptor *ep =
					&id->endpoint[e];
				static const char *tt[] = { "control", "iso",
							    "bulk", "interrupt" };
				int mps = ep->wMaxPacketSize & 0x7ff;
				int mult = ((ep->wMaxPacketSize >> 11) & 3) + 1;

				printf("    ep 0x%02x %-9s %s  mps=%d x%d = %d B"
				       "  interval=%d\n",
				       ep->bEndpointAddress,
				       tt[ep->bmAttributes & 3],
				       (ep->bEndpointAddress & 0x80) ? "IN " : "OUT",
				       mps, mult, mps * mult, ep->bInterval);

				/* SuperSpeed companion carries the burst size --
				 * its presence is how we know we're on a 5G link */
				if (ep->extra_length > 0) {
					struct libusb_ss_endpoint_companion_descriptor *ss;

					if (libusb_get_ss_endpoint_companion_descriptor(
						    NULL, ep, &ss) == 0) {
						printf("      SS companion: bMaxBurst=%d"
						       " (%d pkts) bytes/interval=%d\n",
						       ss->bMaxBurst,
						       ss->bMaxBurst + 1,
						       ss->wBytesPerInterval);
						libusb_free_ss_endpoint_companion_descriptor(ss);
					}
				}
			}
		}
	}
	libusb_free_config_descriptor(cfg);
}

/* Listen on the interrupt endpoint. The device appears to volunteer status
 * without being asked, so this tells us the unsolicited event format. */
static int listen_interrupt(libusb_device_handle *h, int seconds)
{
	uint8_t buf[HD60S_STATUS_PKT_MAX];
	double t0 = now_s(), last = -1;
	int n = 0, rc;

	rc = libusb_claim_interface(h, HD60S_IF_STATUS);
	if (rc != 0) {
		log_err("claim interface %d: %s", HD60S_IF_STATUS,
			libusb_error_name(rc));
		return -1;
	}

	log_info("listening on ep 0x%02x for %ds (ctrl-c to stop early)",
		 HD60S_EP_STATUS_INT, seconds);

	while (!stop && now_s() - t0 < seconds) {
		int len = 0;

		rc = libusb_interrupt_transfer(h, HD60S_EP_STATUS_INT, buf,
					       sizeof(buf), &len, 500);
		if (rc == LIBUSB_ERROR_TIMEOUT)
			continue;
		if (rc != 0) {
			log_warn("interrupt transfer: %s", libusb_error_name(rc));
			break;
		}
		if (len == 0)
			continue;

		double t = now_s() - t0;

		printf("\n[%8.3fs] interrupt IN, %d byte(s)\n", t, len);
		if (last >= 0)
			printf("           (+%.3fs since previous)\n", t - last);
		hd60s_hexdump(stdout, "           ", buf, len);
		last = t;
		n++;
	}

	libusb_release_interface(h, HD60S_IF_STATUS);
	log_info("captured %d interrupt packet(s)", n);
	return n;
}

/* Read-only EP0 sweep. Finds which vendor bRequest codes exist by looking for
 * the ones that return data instead of stalling. Purely device-to-host. */
static void sweep_ep0(libusb_device_handle *h, uint8_t rtype)
{
	uint8_t buf[64];
	int found = 0, req;

	printf("\n=== EP0 vendor IN sweep, bmRequestType=0x%02x ===\n", rtype);
	printf("(device-to-host only; nothing is written)\n");

	for (req = 0; req <= 0xff && !stop; req++) {
		int rc = libusb_control_transfer(h, rtype, (uint8_t)req,
						 0, 0, buf, sizeof(buf), 300);
		if (rc > 0) {
			printf("\nbRequest 0x%02x -> %d byte(s)\n", req, rc);
			hd60s_hexdump(stdout, "    ", buf, rc);
			found++;
		} else if (rc == 0) {
			printf("bRequest 0x%02x -> zero-length ACK\n", req);
			found++;
		}
		/* stall (PIPE) is the expected answer for unimplemented codes */
	}
	printf("\n%d responding request code(s) for type 0x%02x\n", found, rtype);
}

int main(int argc, char **argv)
{
	libusb_context *ctx = NULL;
	libusb_device_handle *h;
	libusb_device *dev;
	int rc, seconds = 10, do_sweep = 0, i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--sweep"))
			do_sweep = 1;
		else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
			seconds = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-v"))
			hd60s_log_level = LOG_DEBUG;
		else {
			fprintf(stderr,
				"usage: %s [--seconds N] [--sweep] [-v]\n"
				"  --seconds N  listen on the interrupt ep for N seconds (default 10)\n"
				"  --sweep      also probe which EP0 vendor IN requests respond\n",
				argv[0]);
			return 2;
		}
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	rc = libusb_init(&ctx);
	if (rc != 0) {
		log_err("libusb_init: %s", libusb_error_name(rc));
		return 1;
	}

	h = libusb_open_device_with_vid_pid(ctx, HD60S_VID, HD60S_PID);
	if (!h) {
		log_err("no %04x:%04x found, or permission denied.", HD60S_VID,
			HD60S_PID);
		log_err("install udev/99-elgato-hd60s.rules, or run with sudo.");
		libusb_exit(ctx);
		return 1;
	}
	dev = libusb_get_device(h);

	log_info("opened %04x:%04x on bus %d device %d", HD60S_VID, HD60S_PID,
		 libusb_get_bus_number(dev), libusb_get_device_address(dev));

	int sp = libusb_get_device_speed(dev);
	log_info("link speed: %s", speed_str(sp));
	if (sp < LIBUSB_SPEED_SUPER)
		log_warn("NOT SuperSpeed -- this is the USB 2.0 fallback. "
			 "Bulk capture endpoints only appear on a 5G link.");

	libusb_set_auto_detach_kernel_driver(h, 1);
	dump_active_config(dev);

	if (do_sweep) {
		sweep_ep0(h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN |
				     LIBUSB_RECIPIENT_DEVICE);
		sweep_ep0(h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN |
				     LIBUSB_RECIPIENT_INTERFACE);
	}

	if (seconds > 0)
		listen_interrupt(h, seconds);

	libusb_close(h);
	libusb_exit(ctx);
	return 0;
}
