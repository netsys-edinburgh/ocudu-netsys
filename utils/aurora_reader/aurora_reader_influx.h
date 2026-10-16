/*
 * aurora_reader InfluxDB output (OCUDU).
 *
 * Turns the reader's CSV header and rows into InfluxDB line protocol and sends
 * it to a local Telegraf (inputs.socket_listener, data_format = "influx"),
 * which batches, retries and forwards it to InfluxDB. Reusing the CSV writer
 * keeps both outputs identical: every CSV column becomes one field, grouped
 * into aurora_* measurements by column prefix.
 */

#ifndef AURORA_READER_INFLUX_H
#define AURORA_READER_INFLUX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Opens the connection to Telegraf. @url is one of udp://host:port,
 * tcp://host:port or unix:///path (stream socket). A stream connection that
 * cannot be opened yet is retried on later sends. Returns 0 on success, -1 on
 * an invalid URL.
 */
int aurora_influx_open(const char *url);

/*
 * Adds a tag written on every point. Tag keys and values are escaped here.
 * Call before aurora_influx_set_header().
 */
void aurora_influx_add_tag(const char *key, const char *value);

/*
 * Adds the tags of a comma-separated "key=value,key=value" list (e.g. the
 * AURORA_INFLUX_TAGS environment variable). Malformed entries are skipped.
 */
void aurora_influx_add_tags(const char *list);

/*
 * Builds the column to measurement/field layout from the CSV header line
 * (without trailing newline). @thread_prefixes[i] is "THREAD_<safe name>_"
 * of thread column group i and @thread_names[i] the thread's real name, used
 * as the value of the "thread" tag. Returns 0 on success.
 */
int aurora_influx_set_header(const char *header,
                             const char *const *thread_prefixes,
                             const char *const *thread_names,
                             size_t nof_threads);

/*
 * Converts one CSV row (same layout as the header, without trailing newline,
 * first column timestamp_us) into line protocol and sends it. Returns 0 when
 * sent, -1 when Telegraf is not reachable (the row is dropped).
 */
int aurora_influx_send_row(const char *row);

/* Points sent / rows dropped since start. */
unsigned long aurora_influx_rows_sent(void);
unsigned long aurora_influx_rows_dropped(void);

void aurora_influx_close(void);

#ifdef __cplusplus
}
#endif

#endif /* AURORA_READER_INFLUX_H */
