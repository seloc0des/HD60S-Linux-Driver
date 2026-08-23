/* hd60s-regprobe -- probe the CLASS-interface request space and register banks.
 *
 * Background: static analysis of the official Windows driver (published by the
 * hd60s-linux project, docs/vendor-driver-analysis.md) shows the HD60 S is
 * driven with URB_FUNCTION_CLASS_INTERFACE requests -- bmRequestType 0xA1 for
 * IN and 0x21 for OUT -- not the vendor requests we originally swept. Request
 * 0xc0 is described as a direct register accessor with wValue as the register
 * bank and wIndex as the register address, one byte of payload.
 *
 * That is third-party analysis, not something we have confirmed. Everything
 * here is a READ, so a wrong guess costs a stall, not a device.
 *
 * SAFETY: IN direction only (bmRequestType bit 7 set). No writes anywhere.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libusb-1.0/libusb.h>

#include "../src/hd60s.h"

/* class, device-to-host, interface recipient */
#define RQ_CLASS_IN (LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_ENDPOINT_IN | LIBUSB_RECIPIENT_INTERFACE)

static int cls_in(libusb_device_handle *h, uint8_t req, uint16_t val,
		  uint16_t idx, uint8_t *buf, int len)
{
	return libusb_control_transfer(h, RQ_CLASS_IN, req, val, idx, buf,
				       (uint16_t)len, 500);
}

/* Sweep bRequest for a given bmRequestType, wValue/wIndex zero. */
static void sweep(libusb_device_handle *h, uint8_t rtype, const char *label)
{
	uint8_t buf[64];
	int req, found = 0;

	printf("\n=== sweep bmRequestType 0x%02x (%s) ===\n", rtype, label);
	for (req = 0; req <= 0xff; req++) {
		int rc = libusb_control_transfer(h, rtype, (uint8_t)req, 0, 0,
						 buf, sizeof(buf), 300);
		if (rc > 0) {
			printf("  bRequest 0x%02x -> %d byte(s): ", req, rc);
			for (int i = 0; i < rc && i < 16; i++)
				printf("%02x ", buf[i]);
			printf("\n");
			found++;
		} else if (rc == 0) {
			printf("  bRequest 0x%02x -> zero-length ACK\n", req);
			found++;
		}
	}
	printf("  %d responding code(s)\n", found);
}

/* Read one register: request 0xc0, wValue = bank, wIndex = register. */
static int read_reg(libusb_device_handle *h, uint8_t req, uint16_t bank,
		    uint16_t reg, uint8_t *out)
{
	return cls_in(h, req, bank, reg, out, 1);
}

int main(int argc, char **argv)
{
	libusb_context *ctx = NULL;
	libusb_device_handle *h;
	int do_sweep = 0, i, claimed = -1;
	long scan_bank = -1;
	uint8_t reg_req = 0xc0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--sweep"))
			do_sweep = 1;
		else if (!strcmp(argv[i], "--scan-bank") && i + 1 < argc)
			scan_bank = strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--req") && i + 1 < argc)
			reg_req = (uint8_t)strtoul(argv[++i], NULL, 0);
		else {
			fprintf(stderr,
				"usage: %s [--sweep] [--scan-bank N] [--req R]\n"
				"  --sweep       sweep class/interface + other request types\n"
				"  --scan-bank N read registers 0x00-0xff of bank N\n"
				"  --req R       request code for register access (default 0xc0)\n"
				"All transfers are IN (reads). Nothing is written.\n",
				argv[0]);
			return 2;
		}
	}
	if (!do_sweep && scan_bank < 0)
		do_sweep = 1;

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

	/* class-interface requests are usually answered only once the interface
	 * is claimed; try both interfaces and keep whichever works */
	for (i = 0; i <= 1; i++) {
		if (libusb_claim_interface(h, i) == 0) {
			claimed = i;
			log_info("claimed interface %d", i);
			break;
		}
	}
	if (claimed < 0)
		log_warn("could not claim any interface; continuing anyway");

	if (do_sweep) {
		sweep(h, RQ_CLASS_IN, "class, IN, interface");
		sweep(h, LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_ENDPOINT_IN |
				  LIBUSB_RECIPIENT_DEVICE, "class, IN, device");
		sweep(h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN |
				  LIBUSB_RECIPIENT_ENDPOINT, "vendor, IN, endpoint");
	}

	if (scan_bank >= 0) {
		uint8_t v;
		int hits = 0;

		printf("\n=== bank 0x%04lx via request 0x%02x, registers 0x00-0xff ===\n",
		       scan_bank, reg_req);
		for (i = 0; i <= 0xff; i++) {
			int rc = read_reg(h, reg_req, (uint16_t)scan_bank,
					  (uint16_t)i, &v);

			if (rc == 1) {
				printf("  reg 0x%02x = 0x%02x\n", i, v);
				hits++;
			}
		}
		printf("  %d register(s) responded\n", hits);
	}

	if (claimed >= 0)
		libusb_release_interface(h, claimed);
	libusb_close(h);
	libusb_exit(ctx);
	return 0;
}
