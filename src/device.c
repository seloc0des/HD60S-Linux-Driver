/* Real capture source: USB control plane + isochronous streaming.
 *
 * The control primitives here are the ones recovered from the vendor driver and
 * verified on hardware (docs/PROTOCOL.md sec. 5.9-5.12). The bring-up sequence
 * itself is data, not code -- see hd60s_bringup[] -- so it can be extended as
 * more of the vendor init is resolved without touching the transport.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libusb-1.0/libusb.h>

#include "device.h"
#include "frame.h"
#include "hd60s.h"

#define REQ_FPGA 0xC1
#define REQ_I2C 0xC0
#define RT_IN (LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN | LIBUSB_RECIPIENT_DEVICE)
#define RT_OUT (LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT | LIBUSB_RECIPIENT_DEVICE)

#define NUM_XFERS 8
#define PKTS_PER_XFER 64

/* --- control primitives ------------------------------------------------ */

int hd60s_fpga_read(struct hd60s_dev *d, uint8_t reg, uint8_t *out)
{
	*out = 0;
	return libusb_control_transfer(d->h, RT_IN, REQ_FPGA, reg, 0, out, 1,
				       1000) == 1 ? 0 : -1;
}

/* the value rides in the high byte of wValue; there is no data stage */
int hd60s_fpga_write(struct hd60s_dev *d, uint8_t reg, uint8_t val)
{
	uint16_t wval = (uint16_t)((val << 8) | reg);

	return libusb_control_transfer(d->h, RT_OUT, REQ_FPGA, wval, 0, NULL, 0,
				       1000) == 0 ? 0 : -1;
}

int hd60s_mcu_write(struct hd60s_dev *d, uint8_t cmd, const uint8_t *buf, int len)
{
	return libusb_control_transfer(d->h, RT_OUT, REQ_I2C,
				       (uint16_t)(0x5000 | cmd), 0,
				       (uint8_t *)buf, (uint16_t)len,
				       1000) == len ? 0 : -1;
}

int hd60s_mcu_read(struct hd60s_dev *d, uint8_t cmd, uint8_t *buf, int len)
{
	memset(buf, 0, (size_t)len);
	return libusb_control_transfer(d->h, RT_IN, REQ_I2C,
				       (uint16_t)(0x5000 | cmd), 0, buf,
				       (uint16_t)len, 1000) == len ? 0 : -1;
}

int hd60s_i2c_read(struct hd60s_dev *d, uint8_t slave, uint8_t reg, uint8_t *out)
{
	uint8_t pkt[3] = { (uint8_t)(slave | 1), 0x01, reg };

	if (hd60s_mcu_write(d, 0x66, pkt, sizeof(pkt)) < 0)
		return -1;
	usleep(8000);
	return hd60s_mcu_read(d, 0x66, out, 1);
}

int hd60s_i2c_write(struct hd60s_dev *d, uint8_t slave, uint8_t reg, uint8_t val)
{
	uint8_t pkt[4] = { (uint8_t)(slave & 0xfe), 0x02, reg, val };
	int rc = hd60s_mcu_write(d, 0x66, pkt, sizeof(pkt));

	usleep(8000);
	return rc;
}

/* MST3367 access.
 *
 * IMPORTANT: 0x9c is one of i2c_access's *special* slaves (0x88/0x98/0x9c).
 * Those are addressed with the slave value as the MCU command -- wValue
 * 0x509c -- and a raw payload, NOT the generic command-0x66 form used for
 * every other slave. Getting this wrong is silent: the generic form is
 * accepted and returns plausible-looking data, but page selection does not
 * take, so every read comes from whatever window happened to be selected.
 *
 * Verified: writing {0x00, page} via wValue 0x509c produces genuinely
 * different register windows, while the generic form does not change them.
 */
int hd60s_mst_write(struct hd60s_dev *d, uint8_t page, uint8_t reg, uint8_t val)
{
	uint8_t sel[2] = { 0x00, page };
	uint8_t wr[2] = { reg, val };

	if (hd60s_mcu_write(d, HD60S_SLAVE_MST3367, sel, sizeof(sel)) < 0)
		return -1;
	usleep(8000);
	if (hd60s_mcu_write(d, HD60S_SLAVE_MST3367, wr, sizeof(wr)) < 0)
		return -1;
	usleep(8000);
	return 0;
}

/* Reads still use the generic form -- it is what returns correct data, and the
 * page state selected above persists in the chip. */
int hd60s_mst_read(struct hd60s_dev *d, uint8_t page, uint8_t reg, uint8_t *out)
{
	uint8_t sel[2] = { 0x00, page };

	if (hd60s_mcu_write(d, HD60S_SLAVE_MST3367, sel, sizeof(sel)) < 0)
		return -1;
	usleep(8000);
	return hd60s_i2c_read(d, HD60S_SLAVE_MST3367, reg, out);
}

/* CPLD (Altera MAX II, I2C slave 0x98) access. Like the MST3367 it is one of
 * i2c_access's special slaves, so writes go out as wValue 0x5098 with a raw
 * {reg, val} payload. Reads use the generic form.
 *
 * From CY3014_SetFpgaRegister / GetFpgaRegister -- misleadingly named: these
 * are the CPLD byte accessors, not the FX3's bRequest-0xC1 register file.
 */
int hd60s_cpld_write(struct hd60s_dev *d, uint8_t reg, uint8_t val)
{
	uint8_t pkt[2] = { reg, val };
	int rc = hd60s_mcu_write(d, HD60S_SLAVE_CPLD, pkt, sizeof(pkt));

	usleep(8000);
	return rc;
}

int hd60s_cpld_read(struct hd60s_dev *d, uint8_t reg, uint8_t *out)
{
	return hd60s_i2c_read(d, HD60S_SLAVE_CPLD, reg, out);
}

/* Raw MST3367 access that does NOT re-select the page.
 *
 * Needed because MST3367_HDMI_INIT read-modify-writes register 0x00 -- the page
 * register itself -- to set bit 7, and that bit must stay set across the
 * following 0xCE/0xCF writes. Any helper that re-selects the page first
 * destroys the hold, which is exactly what hd60s_mst_write does.
 */
int hd60s_mst_raw_write(struct hd60s_dev *d, uint8_t reg, uint8_t val)
{
	uint8_t pkt[2] = { reg, val };
	int rc = hd60s_mcu_write(d, HD60S_SLAVE_MST3367, pkt, sizeof(pkt));

	usleep(8000);
	return rc;
}

int hd60s_mst_raw_read(struct hd60s_dev *d, uint8_t reg, uint8_t *out)
{
	return hd60s_i2c_read(d, HD60S_SLAVE_MST3367, reg, out);
}

/* Hot-plug detect toward the source: page 0 register 0xB7 bit 1.
 *
 * THE BIT IS ACTIVE LOW. Setting it de-asserts HPD; clearing it asserts.
 * Measured directly: with 0xB7 = 0x00 the receiver reports V total 1125,
 * H total 2200 and reg 0x55 = 0x7f, which satisfies the driver's validity gate
 * ((v & 0x3c) == 0x3c) -- a fully locked 1080p60 signal. With 0xB7 = 0x02 all
 * of that collapses.
 *
 * This also matches the vendor driver, which ends its bring-up with
 * 0xB7 = 0x00 (VMA 0x43735) -- something the disassembly reported and this
 * code previously overrode on a mistaken reading.
 */
int hd60s_set_hpd(struct hd60s_dev *d, int on)
{
	uint8_t v = 0;

	if (hd60s_mst_read(d, 0x00, 0xb7, &v) < 0)
		return -1;
	/* on == assert == bit CLEAR */
	v = on ? (uint8_t)(v & ~0x02) : (uint8_t)(v | 0x02);
	if (hd60s_mst_write(d, 0x00, 0xb7, v) < 0)
		return -1;
	if (hd60s_mst_read(d, 0x00, 0xb7, &v) < 0)
		return -1;
	log_info("HPD %s (reg 0xb7 = 0x%02x, bit is active low)",
		 (v & 0x02) ? "de-asserted" : "ASSERTED", v);
	return ((v & 0x02) == 0) == (on != 0) ? 0 : -1;
}

/* Read the EDID the device presents to the source. Requires HPD asserted. */
int hd60s_read_edid(struct hd60s_dev *d, uint8_t *buf, int len)
{
	int i;

	for (i = 0; i < len; i++)
		if (hd60s_i2c_read(d, 0xa0, (uint8_t)i, &buf[i]) < 0)
			return -1;
	return 0;
}

/* CheckIsMCUExist(), VMA 0x19f80. Prerequisite for everything else: the
 * MST3367 register path only routes through the MCU once this succeeds. */
int hd60s_handshake(struct hd60s_dev *d)
{
	uint8_t v = 0;
	int i;

	if (hd60s_fpga_write(d, 0x39, 0xC0) < 0 ||
	    hd60s_fpga_write(d, 0x34, 0x41) < 0) {
		log_err("handshake: register write rejected");
		return -1;
	}
	for (i = 0; i < 100; i++) {
		usleep(10 * 1000);
		if (hd60s_fpga_read(d, 0x39, &v) == 0 && v == 1) {
			log_info("MCU up after %d poll(s)", i + 1);
			d->mcu_up = 1;
			return 0;
		}
	}
	log_err("handshake: reg 0x39 never read 1 (last 0x%02x)", v);
	return -1;
}

/* --- isochronous streaming --------------------------------------------- */

static void LIBUSB_CALL iso_cb(struct libusb_transfer *x)
{
	struct hd60s_dev *d = x->user_data;
	int i;

	if (x->status == LIBUSB_TRANSFER_CANCELLED ||
	    x->status == LIBUSB_TRANSFER_NO_DEVICE) {
		free(x->buffer);
		libusb_free_transfer(x);
		d->live--;
		return;
	}

	for (i = 0; i < x->num_iso_packets; i++) {
		struct libusb_iso_packet_descriptor *p = &x->iso_packet_desc[i];
		uint8_t *src;

		d->pkts++;
		if (p->status != LIBUSB_TRANSFER_COMPLETED || !p->actual_length)
			continue;

		src = libusb_get_iso_packet_buffer_simple(x, i);
		d->bytes += p->actual_length;
		d->nonempty++;
		if (d->on_data)
			d->on_data(d, src, p->actual_length);
	}

	if (!d->stop && libusb_submit_transfer(x) == 0)
		return;
	free(x->buffer);
	libusb_free_transfer(x);
	d->live--;
}

int hd60s_stream_start(struct hd60s_dev *d, int alt)
{
	int pkt = (alt == 1) ? 1024 : 3072;
	int i, rc;

	rc = libusb_claim_interface(d->h, HD60S_IF_STREAM);
	if (rc != 0) {
		log_err("claim interface: %s", libusb_error_name(rc));
		return -1;
	}
	d->claimed = 1;
	rc = libusb_set_interface_alt_setting(d->h, HD60S_IF_STREAM, alt);
	if (rc != 0) {
		log_err("set alt %d: %s", alt, libusb_error_name(rc));
		return -1;
	}

	for (i = 0; i < NUM_XFERS; i++) {
		size_t len = (size_t)pkt * PKTS_PER_XFER;
		uint8_t *buf = malloc(len);
		struct libusb_transfer *x = libusb_alloc_transfer(PKTS_PER_XFER);

		if (!buf || !x) {
			free(buf);
			if (x)
				libusb_free_transfer(x);
			break;
		}
		libusb_fill_iso_transfer(x, d->h, HD60S_EP_STREAM_ISO, buf,
					 (int)len, PKTS_PER_XFER, iso_cb, d, 1000);
		libusb_set_iso_packet_lengths(x, pkt);
		if (libusb_submit_transfer(x) != 0) {
			free(buf);
			libusb_free_transfer(x);
			continue;
		}
		d->live++;
	}
	if (!d->live) {
		log_err("no isochronous transfers could be submitted");
		return -1;
	}
	log_info("streaming: alt %d, %d B/uframe, %d transfers", alt, pkt, d->live);
	return 0;
}

void hd60s_stream_stop(struct hd60s_dev *d)
{
	struct timeval tv = { 0, 50000 };
	int i;

	d->stop = 1;
	for (i = 0; i < 40 && d->live; i++)
		libusb_handle_events_timeout(d->ctx, &tv);
	if (d->claimed) {
		libusb_set_interface_alt_setting(d->h, HD60S_IF_STREAM, 0);
		libusb_release_interface(d->h, HD60S_IF_STREAM);
		d->claimed = 0;
	}
}

int hd60s_pump(struct hd60s_dev *d, int ms)
{
	struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };

	return libusb_handle_events_timeout(d->ctx, &tv);
}

/* --- open / close ------------------------------------------------------ */

/* quiet: suppress the not-found diagnostics. Used by the retry path, which
 * polls once a second and would otherwise fill the terminal. */
int hd60s_open_quiet(struct hd60s_dev *d, int quiet)
{
	memset(d, 0, sizeof(*d));
	if (libusb_init(&d->ctx) != 0) {
		if (!quiet)
			log_err("libusb_init failed");
		return -1;
	}
	d->h = libusb_open_device_with_vid_pid(d->ctx, HD60S_VID, HD60S_PID);
	if (!d->h) {
		if (!quiet) {
			log_err("no %04x:%04x, or permission denied",
				HD60S_VID, HD60S_PID);
			log_err("install udev/99-elgato-hd60s.rules and replug");
		}
		libusb_exit(d->ctx);
		d->ctx = NULL;
		return -1;
	}
	libusb_set_auto_detach_kernel_driver(d->h, 1);

	if (!quiet &&
	    libusb_get_device_speed(libusb_get_device(d->h)) < LIBUSB_SPEED_SUPER)
		log_warn("link is not SuperSpeed -- the device declares full "
			 "functionality requires it");
	return 0;
}

int hd60s_open(struct hd60s_dev *d)
{
	return hd60s_open_quiet(d, 0);
}

void hd60s_close(struct hd60s_dev *d)
{
	if (d->h)
		libusb_close(d->h);
	if (d->ctx)
		libusb_exit(d->ctx);
	memset(d, 0, sizeof(*d));
}

/* --- scripted bring-up ------------------------------------------------- */

/* The bring-up sequence is still being recovered from the vendor driver, so it
 * lives in a text file rather than in this binary. Iterating on it must not
 * require a rebuild.
 *
 * Grammar, one operation per line, '#' to end of line is a comment:
 *
 *   fpga_write REG VAL          FPGA/CPLD register file (bRequest 0xC1)
 *   fpga_read  REG              read and log
 *   i2c_write  SLAVE REG VAL    via the MCU proxy
 *   i2c_read   SLAVE REG
 *   mst_write  PAGE REG VAL     MST3367 receiver, page-selected
 *   mst_read   PAGE REG
 *   delay      MS
 *   vendor_out REQ WVAL WIDX [bytes...]   raw vendor OUT on EP0
 *   mst_raw    REG VAL          MST3367 write WITHOUT re-selecting the page
 *   mst_rawrmw REG AND OR       ditto, read-modify-write
 *   cpld_write REG VAL          Altera MAX II CPLD (I2C slave 0x98)
 *   cpld_read  REG
 *   cpld_rmw   REG AND OR
 *   mst_rmw    PAGE REG AND OR          v = (v & AND) | OR
 *   poll_mst   PAGE REG MASK WANT [TRIES]   until (val & MASK) == WANT
 *   expect_mst PAGE REG MASK WANT           one-shot check, warns on mismatch
 *
 * Numbers may be decimal or 0x-prefixed.
 */
static int parse_num(const char *s, long *out)
{
	char *end;
	long v = strtol(s, &end, 0);

	if (end == s || *end)
		return -1;
	*out = v;
	return 0;
}

int hd60s_run_script(struct hd60s_dev *d, const char *path)
{
	char line[256];
	FILE *f = fopen(path, "r");
	int lineno = 0, errors = 0, rc;

	if (!f) {
		log_err("cannot open %s: %s", path, strerror(errno));
		return -1;
	}
	log_info("running bring-up script %s", path);

	while (fgets(line, sizeof(line), f)) {
		char *tok[24], *p, *save = NULL;
		int n = 0;
		long a[4];

		lineno++;
		p = strchr(line, '#');
		if (p)
			*p = '\0';
		for (p = strtok_r(line, " \t\r\n", &save);
		     p && n < (int)(sizeof(tok) / sizeof(tok[0]));
		     p = strtok_r(NULL, " \t\r\n", &save))
			tok[n++] = p;
		if (!n)
			continue;

		/* every operand must parse before we touch the device */
		{
			int i, bad = 0;

			for (i = 1; i < n && i <= 4; i++)
				if (parse_num(tok[i], &a[i - 1]) < 0)
					bad = 1;
			if (bad) {
				log_err("%s:%d: bad number", path, lineno);
				errors++;
				continue;
			}
		}

		if (!strcmp(tok[0], "delay") && n == 2) {
			usleep((useconds_t)a[0] * 1000);
		} else if (!strcmp(tok[0], "fpga_write") && n == 3) {
			if (hd60s_fpga_write(d, (uint8_t)a[0], (uint8_t)a[1]) < 0) {
				log_err("%s:%d: fpga_write 0x%02lx = 0x%02lx rejected",
					path, lineno, a[0], a[1]);
				errors++;
			}
		} else if (!strcmp(tok[0], "fpga_read") && n == 2) {
			uint8_t v;

			if (hd60s_fpga_read(d, (uint8_t)a[0], &v) == 0)
				log_info("  fpga 0x%02lx = 0x%02x", a[0], v);
			else
				log_warn("  fpga 0x%02lx unreadable", a[0]);
		} else if (!strcmp(tok[0], "i2c_write") && n == 4) {
			if (hd60s_i2c_write(d, (uint8_t)a[0], (uint8_t)a[1],
					    (uint8_t)a[2]) < 0) {
				log_err("%s:%d: i2c_write rejected", path, lineno);
				errors++;
			}
		} else if (!strcmp(tok[0], "i2c_read") && n == 3) {
			uint8_t v;

			if (hd60s_i2c_read(d, (uint8_t)a[0], (uint8_t)a[1], &v) == 0)
				log_info("  i2c 0x%02lx reg 0x%02lx = 0x%02x",
					 a[0], a[1], v);
		} else if (!strcmp(tok[0], "mst_write") && n == 4) {
			if (hd60s_mst_write(d, (uint8_t)a[0], (uint8_t)a[1],
					    (uint8_t)a[2]) < 0) {
				log_err("%s:%d: mst_write rejected", path, lineno);
				errors++;
			}
		} else if (!strcmp(tok[0], "mst_read") && n == 3) {
			uint8_t v;

			if (hd60s_mst_read(d, (uint8_t)a[0], (uint8_t)a[1], &v) == 0)
				log_info("  mst page 0x%02lx reg 0x%02lx = 0x%02x",
					 a[0], a[1], v);
		} else if (!strcmp(tok[0], "poll_mst") && (n == 5 || n == 6)) {
			long tries = (n == 6) ? a[4 - 1] : 100;
			uint8_t v = 0;
			int i, got = 0;

			/* n==6 puts TRIES in tok[5]; re-parse to keep it simple */
			if (n == 6 && parse_num(tok[5], &tries) < 0)
				tries = 100;
			for (i = 0; i < tries; i++) {
				usleep(10 * 1000);
				if (hd60s_mst_read(d, (uint8_t)a[0], (uint8_t)a[1],
						   &v) == 0 &&
				    (v & (uint8_t)a[2]) == (uint8_t)a[3]) {
					got = 1;
					break;
				}
			}
			log_info("  poll page 0x%02lx reg 0x%02lx -> 0x%02x %s"
				 " after %d", a[0], a[1], v,
				 got ? "MATCH" : "TIMEOUT", i);
			if (!got)
				errors++;
		} else if (!strcmp(tok[0], "vendor_out") && n >= 4) {
			/* vendor_out REQ WVALUE WINDEX [byte...] */
			uint8_t data[64];
			int nd = 0, k;

			for (k = 4; k < n && nd < (int)sizeof(data); k++) {
				long b;

				if (parse_num(tok[k], &b) < 0) {
					log_err("%s:%d: bad payload byte", path, lineno);
					break;
				}
				data[nd++] = (uint8_t)b;
			}
			rc = libusb_control_transfer(d->h,
						     LIBUSB_REQUEST_TYPE_VENDOR |
							     LIBUSB_ENDPOINT_OUT |
							     LIBUSB_RECIPIENT_DEVICE,
						     (uint8_t)a[0], (uint16_t)a[1],
						     (uint16_t)a[2],
						     nd ? data : NULL,
						     (uint16_t)nd, 1000);
			if (nd != n - 4)
				log_warn("  vendor 0x%02lx: parsed %d of %d payload byte(s)",
					 a[0], nd, n - 4);
			log_info("  vendor 0x%02lx wVal=0x%04lx wIdx=0x%04lx len=%d -> %s",
				 a[0], a[1], a[2], nd,
				 rc >= 0 ? "ok" : libusb_error_name(rc));
			if (rc < 0)
				errors++;
		} else if (!strcmp(tok[0], "mst_raw") && n == 3) {
			if (hd60s_mst_raw_write(d, (uint8_t)a[0], (uint8_t)a[1]) < 0) {
				log_err("%s:%d: mst_raw rejected", path, lineno);
				errors++;
			}
		} else if (!strcmp(tok[0], "mst_rawrmw") && n == 4) {
			uint8_t v = 0;

			if (hd60s_mst_raw_read(d, (uint8_t)a[0], &v) == 0) {
				uint8_t nv = (uint8_t)((v & (uint8_t)a[1]) | (uint8_t)a[2]);

				hd60s_mst_raw_write(d, (uint8_t)a[0], nv);
				log_info("  raw rmw r%02lx: %02x -> %02x", a[0], v, nv);
			} else {
				errors++;
			}
		} else if (!strcmp(tok[0], "cpld_write") && n == 3) {
			if (hd60s_cpld_write(d, (uint8_t)a[0], (uint8_t)a[1]) < 0) {
				log_err("%s:%d: cpld_write rejected", path, lineno);
				errors++;
			} else {
				uint8_t back = 0;

				hd60s_cpld_read(d, (uint8_t)a[0], &back);
				log_info("  cpld 0x%02lx <- 0x%02lx (reads 0x%02x)%s",
					 a[0], a[1], back,
					 back == (uint8_t)a[1] ? "" : " MISMATCH");
			}
		} else if (!strcmp(tok[0], "cpld_read") && n == 2) {
			uint8_t v;

			if (hd60s_cpld_read(d, (uint8_t)a[0], &v) == 0)
				log_info("  cpld 0x%02lx = 0x%02x", a[0], v);
		} else if (!strcmp(tok[0], "cpld_rmw") && n == 4) {
			uint8_t v = 0;

			if (hd60s_cpld_read(d, (uint8_t)a[0], &v) == 0) {
				uint8_t nv = (uint8_t)((v & (uint8_t)a[1]) | (uint8_t)a[2]);
				uint8_t back = 0;

				hd60s_cpld_write(d, (uint8_t)a[0], nv);
				hd60s_cpld_read(d, (uint8_t)a[0], &back);
				log_info("  cpld rmw 0x%02lx: %02x -> %02x", a[0], v, back);
			} else {
				errors++;
			}
		} else if (!strcmp(tok[0], "mst_rmw") && n == 5) {
			/* v = (v & AND) | OR -- the vendor init is almost all
			 * read-modify-write, so express it directly */
			uint8_t v = 0;

			if (hd60s_mst_read(d, (uint8_t)a[0], (uint8_t)a[1], &v) < 0) {
				log_err("%s:%d: mst_rmw read failed", path, lineno);
				errors++;
			} else {
				uint8_t nv = (uint8_t)((v & (uint8_t)a[2]) | (uint8_t)a[3]);

				if (hd60s_mst_write(d, (uint8_t)a[0], (uint8_t)a[1],
						    nv) < 0) {
					log_err("%s:%d: mst_rmw write failed",
						path, lineno);
					errors++;
				} else {
					uint8_t back = 0;

					hd60s_mst_read(d, (uint8_t)a[0],
						       (uint8_t)a[1], &back);
					log_info("  rmw p%02lx r%02lx: %02x -> %02x%s",
						 a[0], a[1], v, back,
						 back == nv ? "" : " (WROTE "
						 "DIFFERENT VALUE)");
				}
			}
		} else if (!strcmp(tok[0], "expect_mst") && n == 5) {
			uint8_t v = 0;

			hd60s_mst_read(d, (uint8_t)a[0], (uint8_t)a[1], &v);
			log_info("  expect page 0x%02lx reg 0x%02lx = 0x%02x -> %s",
				 a[0], a[1], v,
				 (v & (uint8_t)a[2]) == (uint8_t)a[3] ? "ok" : "MISMATCH");
		} else {
			log_err("%s:%d: unknown op '%s' with %d arg(s)", path,
				lineno, tok[0], n - 1);
			errors++;
		}
	}
	fclose(f);
	log_info("script done, %d error(s)", errors);
	return errors ? -1 : 0;
}
