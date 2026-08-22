/* Shared definitions for the Elgato Game Capture HD60 S driver.
 *
 * Everything in here is either observed on real hardware or structural.
 * Vendor protocol constants deliberately live in proto.h once confirmed --
 * see docs/PROTOCOL.md. Don't add guessed values here.
 */
#ifndef HD60S_H
#define HD60S_H

#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

#define HD60S_VID 0x0fd9
#define HD60S_PID 0x005e /* original HD60 S only; the S+ is different silicon */

/* USB 2.0 fallback personality (observed). The SuperSpeed layout differs and
 * is filled in once we capture a 5G link -- see docs/PROTOCOL.md sec. 4. */
#define HD60S_IF_STREAM 0    /* alt 0=idle, 1=1024B, 2=3072B per uframe */
#define HD60S_IF_STATUS 1
#define HD60S_EP_STREAM_ISO 0x83
#define HD60S_EP_STATUS_INT 0x81
#define HD60S_STATUS_PKT_MAX 64

/* Alt setting -> bytes per microframe, for bandwidth math. */
#define HD60S_ALT_IDLE 0
#define HD60S_ALT_LOW 1
#define HD60S_ALT_FULL 2

enum hd60s_loglevel { LOG_ERR = 0, LOG_WARN, LOG_INFO, LOG_DEBUG, LOG_TRACE };

extern enum hd60s_loglevel hd60s_log_level;

static inline void hd60s_log(enum hd60s_loglevel lvl, const char *fmt, ...)
{
	static const char *tag[] = { "ERR ", "WARN", "INFO", "DBG ", "TRC " };
	va_list ap;

	if (lvl > hd60s_log_level)
		return;
	fprintf(stderr, "[%s] ", tag[lvl]);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

#define log_err(...) hd60s_log(LOG_ERR, __VA_ARGS__)
#define log_warn(...) hd60s_log(LOG_WARN, __VA_ARGS__)
#define log_info(...) hd60s_log(LOG_INFO, __VA_ARGS__)
#define log_dbg(...) hd60s_log(LOG_DEBUG, __VA_ARGS__)
#define log_trace(...) hd60s_log(LOG_TRACE, __VA_ARGS__)

void hd60s_hexdump(FILE *f, const char *prefix, const uint8_t *buf, int len);

#endif /* HD60S_H */
