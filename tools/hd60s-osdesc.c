/* hd60s-osdesc -- probe the standard descriptor space for anything the vendor
 * sweep would miss. Strictly read-only: every transfer is GET_DESCRIPTOR or
 * another standard IN request.
 *
 * The interesting one is the Microsoft OS String Descriptor at string index
 * 0xEE. Devices that ship Windows drivers frequently expose it, and it *names a
 * vendor request code* which the host then uses to fetch extended compat IDs.
 * If the HD60 S has one, that hands us a vendor bRequest for free -- something
 * blind sweeping cannot do safely.
 *
 * Also enumerates every string index, since vendors leave debug strings around.
 */
#include <stdlib.h>
#include <string.h>

#include <libusb-1.0/libusb.h>

#include "../src/hd60s.h"

#define MS_OS_STRING_INDEX 0xEE
#define MS_FEATURE_COMPAT_ID 0x0004
#define MS_FEATURE_PROPERTIES 0x0005

static int get_desc(libusb_device_handle *h, uint8_t type, uint8_t index,
		    uint16_t langid, uint8_t *buf, int len)
{
	return libusb_control_transfer(h,
				       LIBUSB_ENDPOINT_IN | LIBUSB_RECIPIENT_DEVICE,
				       LIBUSB_REQUEST_GET_DESCRIPTOR,
				       (uint16_t)((type << 8) | index), langid,
				       buf, (uint16_t)len, 1000);
}

/* UTF-16LE string descriptor -> ascii, good enough for identification */
static void show_string(const uint8_t *b, int len)
{
	int i;

	for (i = 2; i + 1 < len; i += 2)
		putchar((b[i] >= 0x20 && b[i] < 0x7f && !b[i + 1]) ? b[i] : '.');
}

int main(void)
{
	libusb_context *ctx = NULL;
	libusb_device_handle *h;
	uint8_t buf[512];
	uint16_t langid = 0x0409;
	int rc, i, found = 0;

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

	/* langid table first -- index 0 with langid 0 */
	rc = get_desc(h, LIBUSB_DT_STRING, 0, 0, buf, sizeof(buf));
	if (rc >= 4) {
		langid = (uint16_t)(buf[2] | (buf[3] << 8));
		printf("supported langid: 0x%04x\n", langid);
	}

	printf("\n=== string descriptors ===\n");
	for (i = 0; i < 256; i++) {
		rc = get_desc(h, LIBUSB_DT_STRING, (uint8_t)i, langid, buf,
			      sizeof(buf));
		if (rc < 2)
			continue;
		if (i == 0)
			continue; /* langid table, not text */
		printf("  [0x%02x] ", i);
		show_string(buf, rc);
		printf("\n");
		found++;
	}
	printf("  (%d string(s))\n", found);

	printf("\n=== Microsoft OS string descriptor (index 0xEE) ===\n");
	rc = get_desc(h, LIBUSB_DT_STRING, MS_OS_STRING_INDEX, 0, buf, sizeof(buf));
	if (rc < 0) {
		printf("  not present (%s)\n", libusb_error_name(rc));
		printf("  -> no Microsoft OS descriptors, so no vendor code to learn here\n");
	} else {
		uint8_t vendor_code;

		printf("  %d bytes: ", rc);
		hd60s_hexdump(stdout, "", buf, rc);
		if (rc >= 18 && !memcmp(buf + 2, "M\0S\0F\0T\0""1\0""0\0""0\0", 14)) {
			vendor_code = buf[16];
			printf("  signature MSFT100 confirmed\n");
			printf("  *** bVendorCode = 0x%02x ***\n", vendor_code);

			/* Extended Compat ID: names the driver Windows would bind */
			rc = libusb_control_transfer(h,
						     LIBUSB_REQUEST_TYPE_VENDOR |
							     LIBUSB_ENDPOINT_IN |
							     LIBUSB_RECIPIENT_DEVICE,
						     vendor_code, 0,
						     MS_FEATURE_COMPAT_ID, buf,
						     sizeof(buf), 1000);
			if (rc > 0) {
				printf("\n  extended compat ID (%d bytes):\n", rc);
				hd60s_hexdump(stdout, "    ", buf, rc);
			} else {
				printf("  extended compat ID: %s\n",
				       libusb_error_name(rc));
			}

			rc = libusb_control_transfer(h,
						     LIBUSB_REQUEST_TYPE_VENDOR |
							     LIBUSB_ENDPOINT_IN |
							     LIBUSB_RECIPIENT_INTERFACE,
						     vendor_code, 0,
						     MS_FEATURE_PROPERTIES, buf,
						     sizeof(buf), 1000);
			if (rc > 0) {
				printf("\n  extended properties (%d bytes):\n", rc);
				hd60s_hexdump(stdout, "    ", buf, rc);
			} else {
				printf("  extended properties: %s\n",
				       libusb_error_name(rc));
			}
		} else {
			printf("  no MSFT100 signature\n");
		}
	}

	printf("\n=== other standard descriptors ===\n");
	{
		static const struct {
			uint8_t type;
			const char *name;
		} types[] = {
			{ 0x06, "DEVICE_QUALIFIER" },
			{ 0x07, "OTHER_SPEED_CONFIG" },
			{ 0x0f, "BOS" },
			{ 0x21, "HID" },
		};

		for (size_t k = 0; k < sizeof(types) / sizeof(types[0]); k++) {
			rc = get_desc(h, types[k].type, 0, 0, buf, sizeof(buf));
			if (rc > 0) {
				printf("  %s (%d bytes):\n", types[k].name, rc);
				hd60s_hexdump(stdout, "    ", buf, rc);
			} else {
				printf("  %-20s %s\n", types[k].name,
				       libusb_error_name(rc));
			}
		}
	}

	libusb_close(h);
	libusb_exit(ctx);
	return 0;
}
