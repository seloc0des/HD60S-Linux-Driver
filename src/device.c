/* Real capture source: USB control plane + isochronous streaming.
 *
 * The control primitives here are the ones recovered from the vendor driver and
 * verified on hardware (docs/PROTOCOL.md sec. 5.9-5.12). The bring-up sequence
 * itself is data, not code -- see hd60s_bringup[] -- so it can be extended as
 * more of the vendor init is resolved without touching the transport.
 */
#include <errno.h>
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

/* MST3367 banks its registers; slave 0x9c register 0x00 selects the page. */
int hd60s_mst_write(struct hd60s_dev *d, uint8_t page, uint8_t reg, uint8_t val)
{
	if (hd60s_i2c_write(d, HD60S_SLAVE_MST3367, 0x00, page) < 0)
		return -1;
	return hd60s_i2c_write(d, HD60S_SLAVE_MST3367, reg, val);
}

int hd60s_mst_read(struct hd60s_dev *d, uint8_t page, uint8_t reg, uint8_t *out)
{
	if (hd60s_i2c_write(d, HD60S_SLAVE_MST3367, 0x00, page) < 0)
		return -1;
	return hd60s_i2c_read(d, HD60S_SLAVE_MST3367, reg, out);
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

int hd60s_open(struct hd60s_dev *d)
{
	memset(d, 0, sizeof(*d));
	if (libusb_init(&d->ctx) != 0) {
		log_err("libusb_init failed");
		return -1;
	}
	d->h = libusb_open_device_with_vid_pid(d->ctx, HD60S_VID, HD60S_PID);
	if (!d->h) {
		log_err("no %04x:%04x, or permission denied", HD60S_VID, HD60S_PID);
		log_err("install udev/99-elgato-hd60s.rules and replug");
		libusb_exit(d->ctx);
		return -1;
	}
	libusb_set_auto_detach_kernel_driver(d->h, 1);

	if (libusb_get_device_speed(libusb_get_device(d->h)) < LIBUSB_SPEED_SUPER)
		log_warn("link is not SuperSpeed -- the device declares full "
			 "functionality requires it");
	return 0;
}

void hd60s_close(struct hd60s_dev *d)
{
	if (d->h)
		libusb_close(d->h);
	if (d->ctx)
		libusb_exit(d->ctx);
	memset(d, 0, sizeof(*d));
}
