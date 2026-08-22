#include "hd60s.h"

enum hd60s_loglevel hd60s_log_level = LOG_INFO;

/* classic 16-byte-per-line dump with ascii gutter */
void hd60s_hexdump(FILE *f, const char *prefix, const uint8_t *buf, int len)
{
	int i, j;

	for (i = 0; i < len; i += 16) {
		fprintf(f, "%s%04x  ", prefix, i);
		for (j = 0; j < 16; j++) {
			if (i + j < len)
				fprintf(f, "%02x ", buf[i + j]);
			else
				fputs("   ", f);
			if (j == 7)
				fputc(' ', f);
		}
		fputs(" |", f);
		for (j = 0; j < 16 && i + j < len; j++) {
			uint8_t c = buf[i + j];
			fputc((c >= 0x20 && c < 0x7f) ? c : '.', f);
		}
		fputs("|\n", f);
	}
}
