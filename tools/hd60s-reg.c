/* hd60s-reg -- register access using the protocol recovered from the vendor
 * driver (CY3014.X64.SYS, shipped with debug symbols in the Elgato Game
 * Capture HD installer).
 *
 * Recovered from CY3014_GetSlaveDeviceRegister/SetSlaveDeviceRegister callers
 * at .text 0x62c00 / 0x62c40, both of which call
 * CUSBBaseDevice::SendVendorCommandRequest(bRequest, wValue, wIndex, buf, len, dir):
 *
 *   READ   bmRequestType 0xC0, bRequest 0xC1, wValue = reg,               len 1
 *   WRITE  bmRequestType 0x40, bRequest 0xC1, wValue = (value << 8) | reg, len 0
 *
 * Note the write carries its value in the HIGH byte of wValue and sends no data
 * stage at all -- which is why blind sweeps with wValue=0 never found this.
 *
 * Reads are the default. Writing requires --write and is what the vendor driver
 * itself does during init, but it is still a device state change.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libusb-1.0/libusb.h>

#include "../src/hd60s.h"

#define REQ_REG 0xC1
#define RT_IN (LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN | LIBUSB_RECIPIENT_DEVICE)
#define RT_OUT (LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT | LIBUSB_RECIPIENT_DEVICE)

static int reg_read(libusb_device_handle *h, uint8_t reg, uint8_t *out)
{
	*out = 0;
	return libusb_control_transfer(h, RT_IN, REQ_REG, reg, 0, out, 1, 1000);
}

static int reg_write(libusb_device_handle *h, uint8_t reg, uint8_t val)
{
	uint16_t wval = (uint16_t)((val << 8) | reg);

	return libusb_control_transfer(h, RT_OUT, REQ_REG, wval, 0, NULL, 0, 1000);
}

/* The MCU-presence handshake, transcribed from CheckIsMCUExist() in
 * CY3014.X64.SYS (.text 0x19f80). The driver picks probe register 0x33 for one
 * other VID/PID and 0x39 for everything else, including 0fd9:005e.
 *
 *   write reg 0x39 = 0xC0        (wValue 0xc039)
 *   write reg 0x34 = 0x41        (wValue 0x4134)
 *   poll reg 0x39 up to 100 times, 10 ms apart, until it reads 1
 */
static int handshake(libusb_device_handle *h)
{
	uint8_t v = 0;
	int i, rc;

	log_info("handshake: write reg 0x39 = 0xC0");
	rc = reg_write(h, 0x39, 0xC0);
	if (rc != 0) {
		log_err("  rejected: %s", libusb_error_name(rc));
		return -1;
	}
	log_info("handshake: write reg 0x34 = 0x41");
	rc = reg_write(h, 0x34, 0x41);
	if (rc != 0) {
		log_err("  rejected: %s", libusb_error_name(rc));
		return -1;
	}

	for (i = 0; i < 100; i++) {
		usleep(10 * 1000);
		rc = reg_read(h, 0x39, &v);
		if (rc == 1 && v == 1) {
			log_info("handshake: reg 0x39 = 1 after %d poll(s) -- MCU up", i + 1);
			return 0;
		}
	}
	log_warn("handshake: reg 0x39 never read 1 (last rc=%d val=0x%02x)", rc, v);
	return -1;
}

int main(int argc, char **argv)
{
	libusb_context *ctx = NULL;
	libusb_device_handle *h;
	int i, do_write = 0, scan = 1, rc, do_hs = 0;
	long reg = -1, val = -1;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--reg") && i + 1 < argc) {
			reg = strtol(argv[++i], NULL, 0);
			scan = 0;
		} else if (!strcmp(argv[i], "--write") && i + 1 < argc) {
			val = strtol(argv[++i], NULL, 0);
			do_write = 1;
		} else if (!strcmp(argv[i], "--handshake")) {
			do_hs = 1;
		} else {
			fprintf(stderr,
				"usage: %s [--handshake] [--reg R [--write V]]\n"
				"  --handshake: run the MCU-presence handshake first\n"
				"  no args   : scan registers 0x00-0xff (reads only)\n"
				"  --reg R   : read register R\n"
				"  --write V : write V to register R (device state change)\n",
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

	if (do_hs)
		handshake(h);

	if (scan) {
		int hits = 0;

		printf("scanning registers via bRequest 0x%02x (reads only)\n", REQ_REG);
		for (i = 0; i <= 0xff; i++) {
			uint8_t v;

			rc = reg_read(h, (uint8_t)i, &v);
			if (rc == 1) {
				printf("  reg 0x%02x = 0x%02x\n", i, v);
				hits++;
			}
		}
		printf("  %d register(s) responded\n", hits);
	} else if (do_write) {
		log_warn("writing 0x%02lx to register 0x%02lx", val, reg);
		rc = reg_write(h, (uint8_t)reg, (uint8_t)val);
		printf("write -> %s\n", rc == 0 ? "ACCEPTED" : libusb_error_name(rc));
	} else {
		uint8_t v;

		rc = reg_read(h, (uint8_t)reg, &v);
		if (rc == 1)
			printf("reg 0x%02lx = 0x%02x\n", reg, v);
		else
			printf("reg 0x%02lx -> %s\n", reg, libusb_error_name(rc));
	}

	libusb_close(h);
	libusb_exit(ctx);
	return 0;
}
