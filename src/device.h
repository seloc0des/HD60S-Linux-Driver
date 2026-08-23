#ifndef HD60S_DEVICE_H
#define HD60S_DEVICE_H

#include <stdint.h>

struct libusb_context;
struct libusb_device_handle;
struct hd60s_dev;

/* I2C slaves, recovered from the vendor driver's i2c_access call sites */
#define HD60S_SLAVE_AUDIO 0x94   /* CS53L21 */
#define HD60S_SLAVE_CPLD 0x98    /* Altera MAX II */
#define HD60S_SLAVE_IT6612 0x9a  /* HDMI transmitter (passthrough) */
#define HD60S_SLAVE_MST3367 0x9c /* HDMI receiver */

typedef void (*hd60s_data_cb)(struct hd60s_dev *d, const uint8_t *buf, int len);

struct hd60s_dev {
	struct libusb_context *ctx;
	struct libusb_device_handle *h;
	int mcu_up;
	int claimed;
	int live;               /* transfers in flight */
	volatile int stop;
	unsigned long long pkts, nonempty, bytes;
	hd60s_data_cb on_data;
	void *user;
};

int hd60s_open(struct hd60s_dev *d);
void hd60s_close(struct hd60s_dev *d);

int hd60s_fpga_read(struct hd60s_dev *d, uint8_t reg, uint8_t *out);
int hd60s_fpga_write(struct hd60s_dev *d, uint8_t reg, uint8_t val);
int hd60s_mcu_write(struct hd60s_dev *d, uint8_t cmd, const uint8_t *buf, int len);
int hd60s_mcu_read(struct hd60s_dev *d, uint8_t cmd, uint8_t *buf, int len);
int hd60s_i2c_read(struct hd60s_dev *d, uint8_t slave, uint8_t reg, uint8_t *out);
int hd60s_i2c_write(struct hd60s_dev *d, uint8_t slave, uint8_t reg, uint8_t val);
int hd60s_mst_read(struct hd60s_dev *d, uint8_t page, uint8_t reg, uint8_t *out);
int hd60s_mst_write(struct hd60s_dev *d, uint8_t page, uint8_t reg, uint8_t val);
int hd60s_handshake(struct hd60s_dev *d);

int hd60s_stream_start(struct hd60s_dev *d, int alt);
void hd60s_stream_stop(struct hd60s_dev *d);
int hd60s_pump(struct hd60s_dev *d, int ms);

#endif /* HD60S_DEVICE_H */
