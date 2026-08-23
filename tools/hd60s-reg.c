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

#define REQ_REG 0xC1   /* FPGA/CPLD register file */
#define REQ_I2C 0xC0   /* I2C bridge to the peripheral chips */
#define RT_IN (LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN | LIBUSB_RECIPIENT_DEVICE)
#define RT_OUT (LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT | LIBUSB_RECIPIENT_DEVICE)

/* rlen is normally 1 -- the vendor driver always asks for exactly one byte.
 * It is adjustable only so we can show that asking for more is what made the
 * original blind sweep miss this command. */
static int reg_read_n(libusb_device_handle *h, uint8_t reg, uint8_t *out, int rlen)
{
	memset(out, 0, (size_t)rlen);
	return libusb_control_transfer(h, RT_IN, REQ_REG, reg, 0, out,
				       (uint16_t)rlen, 1000);
}

static int reg_read(libusb_device_handle *h, uint8_t reg, uint8_t *out)
{
	return reg_read_n(h, reg, out, 1);
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

/* I2C via the MCU proxy -- this is the path that actually works.
 *
 * From i2c_access() (VMA 0x19ca0): the packet's first byte becomes the MCU
 * command. Slaves 0x88/0x98/0x9c get their own command value; everything else
 * goes through command 0x66 with the slave as the first payload byte. The
 * generic form is what reaches the peripheral chips:
 *
 *   write  wValue 0x5066 <- [slave|1] [count] [reg]
 *   read   wValue 0x5066 -> count bytes
 *
 * The direct form (bRequest 0xC0 with wValue < 0x100) returns a constant and is
 * not real I2C -- see docs/PROTOCOL.md.
 */
static int i2c_read(libusb_device_handle *h, uint8_t slave, uint8_t reg,
		    uint8_t *out)
{
	uint8_t pkt[3] = { (uint8_t)(slave | 1), 0x01, reg };
	int rc;

	rc = libusb_control_transfer(h, RT_OUT, REQ_I2C, 0x5066, 0, pkt,
				     sizeof(pkt), 1000);
	if (rc != (int)sizeof(pkt))
		return -1;
	usleep(8000);
	*out = 0;
	rc = libusb_control_transfer(h, RT_IN, REQ_I2C, 0x5066, 0, out, 1, 1000);
	return rc == 1 ? 0 : -1;
}

/* I2C write: [slave] [count] [reg] [value...]. The write address is the slave
 * with bit 0 clear, which is how i2c_access builds it. */
static int i2c_write(libusb_device_handle *h, uint8_t slave, uint8_t reg,
		     uint8_t val)
{
	uint8_t pkt[4] = { (uint8_t)(slave & 0xfe), 0x02, reg, val };
	int rc;

	rc = libusb_control_transfer(h, RT_OUT, REQ_I2C, 0x5066, 0, pkt,
				     sizeof(pkt), 1000);
	usleep(8000);
	return rc == (int)sizeof(pkt) ? 0 : -1;
}

/* The MST3367 HDMI receiver banks its register file: slave 0x9c register 0x00
 * selects the page. From the writer at VMA 0x65080, which sends {0x00, page}
 * through i2c_access before touching the target register. */
#define MST3367_SLAVE 0x9c

static int mst_read(libusb_device_handle *h, uint8_t page, uint8_t reg,
		    uint8_t *out)
{
	if (i2c_write(h, MST3367_SLAVE, 0x00, page) != 0)
		return -1;
	return i2c_read(h, MST3367_SLAVE, reg, out);
}

static void mst_dump(libusb_device_handle *h, uint8_t page)
{
	int r;

	printf("MST3367 page 0x%02x, registers 0x00-0xff:\n", page);
	for (r = 0; r < 0x100; r++) {
		uint8_t v;

		if ((r % 16) == 0)
			printf("  %02x:", r);
		if (mst_read(h, page, (uint8_t)r, &v) == 0)
			printf(" %02x", v);
		else
			printf(" --");
		if ((r % 16) == 15)
			printf("\n");
	}
}

/* Dump a slave's low register space -- how you identify a chip. */
static void i2c_dump(libusb_device_handle *h, uint8_t slave, int n)
{
	int r;

	printf("slave 0x%02x registers 0x00-0x%02x:\n", slave, n - 1);
	for (r = 0; r < n; r++) {
		uint8_t v;

		if ((r % 16) == 0)
			printf("  %02x:", r);
		if (i2c_read(h, slave, (uint8_t)r, &v) == 0)
			printf(" %02x", v);
		else
			printf(" --");
		if ((r % 16) == 15)
			printf("\n");
	}
	if (n % 16)
		printf("\n");
}

/* EnableMCU() from the vendor driver (VMA 0x1a590). Same shape as the
 * handshake but writes 0x40 to control register 0x34 instead of 0x41 -- bit 0
 * is what separates the two. No poll afterwards. */
static int enable_mcu(libusb_device_handle *h)
{
	int rc;

	log_info("EnableMCU: write reg 0x39 = 0xC0");
	rc = reg_write(h, 0x39, 0xC0);
	if (rc != 0) {
		log_err("  rejected: %s", libusb_error_name(rc));
		return -1;
	}
	log_info("EnableMCU: write reg 0x34 = 0x40");
	rc = reg_write(h, 0x34, 0x40);
	if (rc != 0) {
		log_err("  rejected: %s", libusb_error_name(rc));
		return -1;
	}
	return 0;
}

/* Raw MCU proxy access (see i2c_read for the packet shape). */
static int mcu_write(libusb_device_handle *h, uint8_t cmd, uint8_t *buf, int len)
{
	return libusb_control_transfer(h, RT_OUT, REQ_I2C,
				       (uint16_t)(0x5000 | cmd), 0, buf,
				       (uint16_t)len, 1000);
}

static int mcu_read(libusb_device_handle *h, uint8_t cmd, uint8_t *buf, int len)
{
	memset(buf, 0, (size_t)len);
	return libusb_control_transfer(h, RT_IN, REQ_I2C,
				       (uint16_t)(0x5000 | cmd), 0, buf,
				       (uint16_t)len, 1000);
}

/* GetMCUROMVersion() (VMA 0x1c240). `33 44 55` is the no-valid-reply pattern;
 * the reply buffer is shared rather than per-command, so read promptly. The
 * first exchange after the handshake sometimes returns idle, hence the retry. */
static int mcu_reply_idle(const uint8_t *r)
{
	return r[0] == 0x33 && r[1] == 0x44 && r[2] == 0x55;
}

static void mcu_version(libusb_device_handle *h)
{
	uint8_t req[5] = { 0xab, 0x03, 0x12, 0x34, 0x57 };
	uint8_t rep[3];
	int rc, try_;

	for (try_ = 0; try_ < 5; try_++) {
		rc = mcu_write(h, 0x66, req, sizeof(req));
		if (rc != (int)sizeof(req)) {
			log_err("MCU write rejected: %s",
				rc < 0 ? libusb_error_name(rc) : "short");
			return;
		}
		usleep(150 * 1000);
		rc = mcu_read(h, 0x66, rep, sizeof(rep));
		if (rc != (int)sizeof(rep)) {
			log_err("MCU read failed: %s",
				rc < 0 ? libusb_error_name(rc) : "short");
			return;
		}
		if (!mcu_reply_idle(rep))
			break;
		log_dbg("attempt %d returned the idle pattern, retrying", try_ + 1);
	}

	if (mcu_reply_idle(rep)) {
		printf("MCU reply: 33 44 55 (idle -- no valid reply after %d tries)\n",
		       try_);
		return;
	}
	printf("MCU ROM version: %02x %02x %02x\n", rep[0], rep[1], rep[2]);
	printf("  vendor driver compares against 51 10 27 -> %s\n",
	       (rep[0] == 0x51 && rep[1] == 0x10 && rep[2] == 0x27)
		       ? "MATCH" : "different");
}

/* Walk even (write) addresses. 0xff is the idle bus -- nothing there. */
static void i2c_scan(libusb_device_handle *h)
{
	int slave, hits = 0;

	printf("I2C scan via the MCU proxy (reads only)\n");
	for (slave = 0; slave <= 0xfe; slave += 2) {
		uint8_t v;

		if (i2c_read(h, (uint8_t)slave, 0x00, &v) != 0)
			continue;
		if (v == 0xff)
			continue;  /* bus idle: no device acknowledged */
		printf("  slave 0x%02x  reg 0x00 = 0x%02x\n", slave, v);
		hits++;
	}
	printf("  %d device(s) responded\n", hits);
	if (!hits)
		printf("  (run --handshake first; the MCU must be up)\n");
}

int main(int argc, char **argv)
{
	libusb_context *ctx = NULL;
	libusb_device_handle *h;
	int i, do_write = 0, scan = 1, rc, do_hs = 0, rlen = 1, do_i2c = 0, do_en = 0, do_mcuver = 0;
	long dump_slave = -1, mst_page = -1;
	long reg = -1, val = -1;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--reg") && i + 1 < argc) {
			reg = strtol(argv[++i], NULL, 0);
			scan = 0;
		} else if (!strcmp(argv[i], "--write") && i + 1 < argc) {
			val = strtol(argv[++i], NULL, 0);
			do_write = 1;
		} else if (!strcmp(argv[i], "--len") && i + 1 < argc) {
			rlen = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--mcu-version")) {
			do_mcuver = 1;
			scan = 0;
		} else if (!strcmp(argv[i], "--enable-mcu")) {
			do_en = 1;
		} else if (!strcmp(argv[i], "--i2c-dump") && i + 1 < argc) {
			dump_slave = strtol(argv[++i], NULL, 0);
			scan = 0;
		} else if (!strcmp(argv[i], "--mst-dump") && i + 1 < argc) {
			mst_page = strtol(argv[++i], NULL, 0);
			scan = 0;
		} else if (!strcmp(argv[i], "--i2c-scan")) {
			do_i2c = 1;
			scan = 0;
		} else if (!strcmp(argv[i], "--handshake")) {
			do_hs = 1;
		} else {
			fprintf(stderr,
				"usage: %s [--handshake] [--reg R [--write V]]\n"
				"  --handshake: run the MCU-presence handshake first\n"
				"  --enable-mcu: run EnableMCU (reg 0x39=0xC0, reg 0x34=0x40)\n"
				"  --mcu-version: read the MCU ROM version via the MCU proxy\n"
				"  --i2c-scan : scan I2C slaves via the MCU proxy (reads only)\n"
				"  --i2c-dump S: dump registers 0x00-0x1f of slave S\n"
				"  --mst-dump P: dump MST3367 HDMI receiver page P\n"
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
	if (do_en)
		enable_mcu(h);

	if (do_mcuver) {
		mcu_version(h);
		libusb_close(h);
		libusb_exit(ctx);
		return 0;
	}

	if (mst_page >= 0) {
		mst_dump(h, (uint8_t)mst_page);
		libusb_close(h);
		libusb_exit(ctx);
		return 0;
	}

	if (dump_slave >= 0) {
		i2c_dump(h, (uint8_t)dump_slave, 32);
		libusb_close(h);
		libusb_exit(ctx);
		return 0;
	}

	if (do_i2c) {
		i2c_scan(h);
		libusb_close(h);
		libusb_exit(ctx);
		return 0;
	}

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
		uint8_t v[512];

		if (rlen < 1 || rlen > (int)sizeof(v))
			rlen = 1;
		rc = reg_read_n(h, (uint8_t)reg, v, rlen);
		if (rc > 0)
			printf("reg 0x%02lx (wLength %d) -> %d byte(s), first = 0x%02x\n",
			       reg, rlen, rc, v[0]);
		else
			printf("reg 0x%02lx (wLength %d) -> %s\n", reg, rlen,
			       libusb_error_name(rc));
	}

	libusb_close(h);
	libusb_exit(ctx);
	return 0;
}
