/* hd60s-writetest -- determine whether bRequest 0xA0 accepts writes.
 *
 * Outcome (docs/PROTOCOL.md 5.1): writes ARE honoured, but only within the same
 * 255-byte window as reads -- 0xfb and 0xfc are accepted, 0x100 stalls. So this
 * is a bounded peek/poke window, not the Cypress FX3 boot loader, and there is
 * no route to loading a flash-reader into RAM and dumping the firmware.
 *
 * Kept for regression checking and for re-testing other addresses.
 *
 * Design for safety:
 *   - By default the write is IDEMPOTENT: we read the byte first and write the
 *     exact same value back. If the write is accepted, nothing changed.
 *   - wLength is never 0. In the FX3 boot protocol a zero-length 0xA0 means
 *     "jump to this address", which would crash the running firmware.
 *   - Writing a *different* value requires --value, and it says so loudly.
 *   - SPI flash is never touched. This is volatile RAM only; a replug restores
 *     everything from flash.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libusb-1.0/libusb.h>

#include "../src/hd60s.h"

#define VENDOR_RAM 0xA0
#define RQ_IN (LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN | LIBUSB_RECIPIENT_DEVICE)
#define RQ_OUT (LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT | LIBUSB_RECIPIENT_DEVICE)

static int ram_read(libusb_device_handle *h, uint32_t addr, uint8_t *buf, int len)
{
	return libusb_control_transfer(h, RQ_IN, VENDOR_RAM,
				       (uint16_t)(addr & 0xffff),
				       (uint16_t)(addr >> 16), buf,
				       (uint16_t)len, 1000);
}

static int ram_write(libusb_device_handle *h, uint32_t addr, uint8_t *buf, int len)
{
	if (len <= 0) {
		log_err("refusing zero-length write (that means 'jump' to FX3)");
		return -1;
	}
	return libusb_control_transfer(h, RQ_OUT, VENDOR_RAM,
				       (uint16_t)(addr & 0xffff),
				       (uint16_t)(addr >> 16), buf,
				       (uint16_t)len, 1000);
}

int main(int argc, char **argv)
{
	libusb_context *ctx = NULL;
	libusb_device_handle *h;
	/* last byte of the readable window: furthest from the live vector table */
	uint32_t addr = 0xfb;
	int explicit_value = -1, i, rc, force = 0;
	uint8_t before, after, wr;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--addr") && i + 1 < argc)
			addr = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--value") && i + 1 < argc)
			explicit_value = (int)strtoul(argv[++i], NULL, 0) & 0xff;
		else if (!strcmp(argv[i], "--force"))
			force = 1;
		else {
			fprintf(stderr,
				"usage: %s [--addr A] [--value V]\n"
				"  default: read the byte at A and write the SAME value back,\n"
				"           so an accepted write changes nothing.\n"
				"  --value: write a DIFFERENT byte. Changes device RAM.\n"
				"  --force: write even where we cannot read back to verify.\n",
				argv[0]);
			return 2;
		}
	}

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

	before = 0;
	rc = ram_read(h, addr, &before, 1);
	if (rc != 1) {
		if (!force) {
			log_err("cannot read 0x%08x first (%s) -- aborting", addr,
				rc < 0 ? libusb_error_name(rc) : "short read");
			log_err("a write test is only meaningful if we can verify it;");
			log_err("pass --force to write blind anyway");
			libusb_close(h);
			libusb_exit(ctx);
			return 1;
		}
		log_warn("0x%08x is not readable -- writing blind (--force)", addr);
		if (explicit_value < 0) {
			log_err("--force needs an explicit --value (nothing to echo back)");
			libusb_close(h);
			libusb_exit(ctx);
			return 2;
		}
	} else {
		log_info("0x%08x currently reads 0x%02x", addr, before);
	}

	wr = (explicit_value >= 0) ? (uint8_t)explicit_value : before;
	if (explicit_value >= 0)
		log_warn("writing a DIFFERENT value 0x%02x -- this changes RAM", wr);
	else
		log_info("writing the same value back (idempotent probe)");

	rc = ram_write(h, addr, &wr, 1);
	if (rc == 1) {
		log_info("RESULT: write ACCEPTED (%d byte)", rc);
		log_info("  The OUT direction of 0xA0 is live, but only inside the");
		log_info("  255-byte window: a write at 0x100 stalls, same bound as");
		log_info("  reads. So this is a bounded peek/poke, NOT the FX3 boot");
		log_info("  loader -- a flash reader cannot be loaded and run.");
	} else if (rc == LIBUSB_ERROR_PIPE) {
		log_info("RESULT: write STALLED -- outside the writable window.");
		log_info("  Writes share the read bound (addr + len <= 0xFF).");
	} else if (rc == LIBUSB_ERROR_TIMEOUT) {
		log_err("RESULT: write TIMED OUT -- EP0 is probably wedged now.");
		log_err("  unplug and replug before doing anything else.");
	} else {
		log_warn("RESULT: unexpected: %s", libusb_error_name(rc));
	}

	/* verify nothing moved under us -- only meaningful if it was readable */
	if (ram_read(h, addr, &after, 1) == 1) {
		log_info("0x%08x now reads 0x%02x (%s)", addr, after,
			 after == before ? "unchanged" : "CHANGED");
	} else if (force) {
		/* it was never readable; a failed read-back here means nothing */
		log_info("0x%08x is outside the readable window, so there is no"
			 " read-back to check", addr);
	} else {
		log_warn("read-back failed; device may need a replug");
	}

	libusb_close(h);
	libusb_exit(ctx);
	return 0;
}
