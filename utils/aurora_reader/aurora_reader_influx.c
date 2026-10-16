/*
 * aurora_reader InfluxDB output (OCUDU), see aurora_reader_influx.h.
 *
 * Measurements, by CSV column prefix (first match wins):
 *   aurora_flow     Group 1 traffic flows; tag flow=<bhtx_in|...|mac_dl_bo>, field = column suffix
 *   aurora_thread   THREAD_<name>_<metric>; tag thread=<name>, field = <metric>
 *   aurora_kpm      kpm_<KPM name>; field = KPM name
 *   aurora_f1c, aurora_f1u, aurora_rlc, aurora_pdcp, aurora_gtp, aurora_handover (ho_), aurora_rc (rc_, rc0_),
 *   aurora_csi, aurora_rusage (ru_), aurora_infra (infra_)
 *   aurora_mac      every other column (HARQ, CRC, SINR/CQI/BSR, FAPI, per-UE MAC aggregates, ...)
 * Outside flows, threads and KPMs, the field key is the full CSV column name.
 * Values keep the CSV text; empty and non-finite values are left out.
 */

#include "aurora_reader_influx.h"

#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* Largest UDP datagram sent: line protocol is split at line boundaries below this size. */
#define INFLUX_UDP_CHUNK 32000

typedef enum { TRANSPORT_NONE = 0, TRANSPORT_UDP, TRANSPORT_TCP, TRANSPORT_UNIX } transport_t;

typedef struct {
    char  *key;      /* escaped "measurement,tags" */
    int   *cols;     /* CSV column indices of the fields */
    char **fields;   /* escaped field keys, parallel to cols */
    int    nof_cols;
} series_t;

static transport_t g_transport = TRANSPORT_NONE;
static int         g_fd = -1;
static struct sockaddr_storage g_addr;
static socklen_t   g_addr_len = 0;
static time_t      g_last_connect_attempt = 0;
static int         g_reported_unreachable = 0;

static char   g_tags[1024] = "";   /* escaped ",k=v,k=v" common to all points */
static series_t *g_series = NULL;
static int    g_nof_series = 0;
static int    g_nof_columns = 0;

static unsigned long g_rows_sent = 0, g_rows_dropped = 0;

/* ---------------------------------------------------------------- growable buffer */

typedef struct { char *data; size_t len, cap; } buf_t;

static void buf_reserve(buf_t *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + extra + 1) cap *= 2;
    char *p = realloc(b->data, cap);
    if (!p) { perror("aurora_reader: realloc"); exit(1); }
    b->data = p;
    b->cap  = cap;
}

static void buf_put(buf_t *b, const char *s, size_t n)
{
    buf_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_puts(buf_t *b, const char *s) { buf_put(b, s, strlen(s)); }

/* Appends @s escaping the characters in @special with a backslash. */
static void buf_put_escaped(buf_t *b, const char *s, const char *special)
{
    for (; *s; s++) {
        if (strchr(special, *s)) buf_put(b, "\\", 1);
        buf_put(b, s, 1);
    }
}

static char *escaped_dup(const char *s, const char *special)
{
    buf_t b = {0};
    buf_put_escaped(&b, s, special);
    if (!b.data) return strdup("");
    return b.data;
}

/* ---------------------------------------------------------------- transport */

static int open_stream(void)
{
    int fd = socket(g_addr.ss_family, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (connect(fd, (const struct sockaddr *)&g_addr, g_addr_len) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int aurora_influx_open(const char *url)
{
    char host[256], port[16];
    if (!strncmp(url, "unix://", 7)) {
        struct sockaddr_un *un = (struct sockaddr_un *)&g_addr;
        memset(&g_addr, 0, sizeof(g_addr));
        un->sun_family = AF_UNIX;
        snprintf(un->sun_path, sizeof(un->sun_path), "%s", url + 7);
        g_addr_len = sizeof(*un);
        g_transport = TRANSPORT_UNIX;
    } else {
        int udp = !strncmp(url, "udp://", 6);
        if (!udp && strncmp(url, "tcp://", 6)) {
            fprintf(stderr, "aurora_reader: unsupported --influx-url '%s' (udp://, tcp:// or unix://)\n", url);
            return -1;
        }
        const char *hp    = url + 6;
        const char *colon = strrchr(hp, ':');
        if (!colon || colon == hp || (size_t)(colon - hp) >= sizeof(host)) {
            fprintf(stderr, "aurora_reader: --influx-url '%s' needs host:port\n", url);
            return -1;
        }
        snprintf(host, sizeof(host), "%.*s", (int)(colon - hp), hp);
        snprintf(port, sizeof(port), "%s", colon + 1);
        struct addrinfo hints = {0}, *res = NULL;
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM;
        int rc = getaddrinfo(host, port, &hints, &res);
        if (rc != 0 || !res) {
            fprintf(stderr, "aurora_reader: cannot resolve '%s': %s\n", url, gai_strerror(rc));
            return -1;
        }
        memcpy(&g_addr, res->ai_addr, res->ai_addrlen);
        g_addr_len = res->ai_addrlen;
        freeaddrinfo(res);
        g_transport = udp ? TRANSPORT_UDP : TRANSPORT_TCP;
    }

    if (g_transport == TRANSPORT_UDP) {
        g_fd = socket(g_addr.ss_family, SOCK_DGRAM, 0);
        if (g_fd < 0 || connect(g_fd, (const struct sockaddr *)&g_addr, g_addr_len) != 0) {
            perror("aurora_reader: influx udp socket");
            return -1;
        }
    } else {
        /* Telegraf may start after the reader: connect lazily and keep retrying. */
        g_fd = open_stream();
        g_last_connect_attempt = time(NULL);
    }
    return 0;
}

/* Sends all of @data on the stream socket. Returns 0 on success. */
static int send_stream(const char *data, size_t len)
{
    if (g_fd < 0) {
        time_t now = time(NULL);
        if (now == g_last_connect_attempt) return -1; /* at most one attempt per second */
        g_last_connect_attempt = now;
        g_fd = open_stream();
        if (g_fd < 0) return -1;
    }
    while (len > 0) {
        ssize_t n = send(g_fd, data, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(g_fd);
            g_fd = -1;
            return -1;
        }
        data += n;
        len  -= (size_t)n;
    }
    return 0;
}

/* Sends @data as UDP datagrams split at line boundaries. Returns 0 when every datagram was sent. */
static int send_udp(const char *data, size_t len)
{
    int ok = 0;
    while (len > 0) {
        size_t chunk = len;
        if (chunk > INFLUX_UDP_CHUNK) {
            chunk = INFLUX_UDP_CHUNK;
            while (chunk > 0 && data[chunk - 1] != '\n') chunk--;
            if (chunk == 0) { /* a single line longer than the chunk: send it whole */
                const char *nl = memchr(data, '\n', len);
                chunk = nl ? (size_t)(nl - data) + 1 : len;
            }
        }
        if (send(g_fd, data, chunk, 0) < 0) ok = -1;
        data += chunk;
        len  -= chunk;
    }
    return ok;
}

/* ---------------------------------------------------------------- tags */

void aurora_influx_add_tag(const char *key, const char *value)
{
    if (!key || !*key || !value || !*value) return;
    buf_t b = {0};
    buf_puts(&b, g_tags);
    buf_puts(&b, ",");
    buf_put_escaped(&b, key, ", =");
    buf_puts(&b, "=");
    buf_put_escaped(&b, value, ", =");
    if (b.len < sizeof(g_tags))
        memcpy(g_tags, b.data, b.len + 1);
    else
        fprintf(stderr, "aurora_reader: influx tag '%s' dropped (tag set too long)\n", key);
    free(b.data);
}

void aurora_influx_add_tags(const char *list)
{
    if (!list) return;
    char *copy = strdup(list), *save = NULL;
    for (char *kv = strtok_r(copy, ",", &save); kv; kv = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(kv, '=');
        if (!eq) continue;
        *eq = '\0';
        aurora_influx_add_tag(kv, eq + 1);
    }
    free(copy);
}

/* ---------------------------------------------------------------- layout */

static const char *const k_flows[] = {"bhtx_in", "bhtx_out", "bhrx_in", "bhrx_out", "mhtx_in",  "mhtx_out", "mhrx_in",
                                      "mhrx_out", "f1u_rlc", "rlc_f1", "rlc_mac", "mac_rlc", "mac_dl_bo"};

static const struct { const char *prefix, *measurement; } k_prefixes[] = {
    {"f1c_", "aurora_f1c"},   {"f1u_", "aurora_f1u"},     {"rlc_", "aurora_rlc"},       {"pdcp_", "aurora_pdcp"},
    {"gtp_", "aurora_gtp"},   {"ho_", "aurora_handover"}, {"rc0_", "aurora_rc"},        {"rc_", "aurora_rc"},
    {"csi_", "aurora_csi"},   {"ru_", "aurora_rusage"},   {"infra_", "aurora_infra"},
};

/* Returns the series index for (measurement, optional extra tag), creating it. */
static int series_for(const char *measurement, const char *tag_key, const char *tag_value)
{
    buf_t key = {0};
    buf_put_escaped(&key, measurement, ", ");
    buf_puts(&key, g_tags);
    if (tag_key) {
        buf_puts(&key, ",");
        buf_put_escaped(&key, tag_key, ", =");
        buf_puts(&key, "=");
        buf_put_escaped(&key, tag_value, ", =");
    }
    for (int i = 0; i < g_nof_series; i++) {
        if (!strcmp(g_series[i].key, key.data)) {
            free(key.data);
            return i;
        }
    }
    series_t *s = realloc(g_series, sizeof(series_t) * (size_t)(g_nof_series + 1));
    if (!s) { perror("aurora_reader: realloc"); exit(1); }
    g_series = s;
    g_series[g_nof_series] = (series_t){key.data, NULL, NULL, 0};
    return g_nof_series++;
}

static void series_add(int idx, int col, const char *field)
{
    series_t *s = &g_series[idx];
    s->cols   = realloc(s->cols, sizeof(int) * (size_t)(s->nof_cols + 1));
    s->fields = realloc(s->fields, sizeof(char *) * (size_t)(s->nof_cols + 1));
    if (!s->cols || !s->fields) { perror("aurora_reader: realloc"); exit(1); }
    s->cols[s->nof_cols]   = col;
    s->fields[s->nof_cols] = escaped_dup(field, ", =");
    s->nof_cols++;
}

static void map_column(int col, const char *name, const char *const *thread_prefixes, const char *const *thread_names,
                       size_t nof_threads)
{
    /* Traffic flows: "<flow>_<stat>", plus the bare "mac_dl_bo" column. */
    for (size_t i = 0; i < sizeof(k_flows) / sizeof(k_flows[0]); i++) {
        size_t n = strlen(k_flows[i]);
        if (!strncmp(name, k_flows[i], n) && (name[n] == '\0' || name[n] == '_')) {
            series_add(series_for("aurora_flow", "flow", k_flows[i]), col, name[n] ? name + n + 1 : "value");
            return;
        }
    }
    /* Longest match: a duplicated name's "THREAD_gnb_103_" columns also start with the first one's "THREAD_gnb_". */
    size_t best = nof_threads, best_len = 0;
    for (size_t i = 0; i < nof_threads; i++) {
        size_t n = strlen(thread_prefixes[i]);
        if (n > best_len && !strncmp(name, thread_prefixes[i], n)) {
            best     = i;
            best_len = n;
        }
    }
    if (best < nof_threads) {
        series_add(series_for("aurora_thread", "thread", thread_names[best]), col, name + best_len);
        return;
    }
    if (!strncmp(name, "kpm_", 4)) {
        series_add(series_for("aurora_kpm", NULL, NULL), col, name + 4);
        return;
    }
    for (size_t i = 0; i < sizeof(k_prefixes) / sizeof(k_prefixes[0]); i++) {
        if (!strncmp(name, k_prefixes[i].prefix, strlen(k_prefixes[i].prefix))) {
            series_add(series_for(k_prefixes[i].measurement, NULL, NULL), col, name);
            return;
        }
    }
    series_add(series_for("aurora_mac", NULL, NULL), col, name);
}

int aurora_influx_set_header(const char *header, const char *const *thread_prefixes, const char *const *thread_names,
                             size_t nof_threads)
{
    char *copy = strdup(header), *save = NULL;
    int   col  = 0;
    for (char *name = strtok_r(copy, ",", &save); name; name = strtok_r(NULL, ",", &save), col++) {
        if (col == 0) continue; /* timestamp_us */
        map_column(col, name, thread_prefixes, thread_names, nof_threads);
    }
    free(copy);
    g_nof_columns = col;
    return col > 1 ? 0 : -1;
}

/* ---------------------------------------------------------------- rows */

/* Appends "field=value"; returns 1 when written. */
static int put_field(buf_t *b, int first, const char *field, const char *value)
{
    if (!value || !*value) return 0;
    char  *end = NULL;
    double v   = strtod(value, &end);
    int    numeric = end != value && *end == '\0';
    if (numeric && !isfinite(v)) return 0;
    if (!first) buf_puts(b, ",");
    buf_puts(b, field);
    buf_puts(b, "=");
    if (numeric) {
        buf_puts(b, value);
    } else {
        buf_puts(b, "\"");
        buf_put_escaped(b, value, "\"\\");
        buf_puts(b, "\"");
    }
    return 1;
}

int aurora_influx_send_row(const char *row)
{
    if (g_transport == TRANSPORT_NONE || g_nof_columns == 0) return -1;

    /* Split the row into its column values. CSV values never contain commas. */
    char  *copy   = strdup(row);
    char **values = calloc((size_t)g_nof_columns, sizeof(char *));
    if (!copy || !values) { perror("aurora_reader: alloc"); exit(1); }
    int n = 0;
    for (char *p = copy; n < g_nof_columns; n++) {
        values[n] = p;
        char *c = strchr(p, ',');
        if (!c) { n++; break; }
        *c = '\0';
        p  = c + 1;
    }

    /* timestamp_us -> nanoseconds (Telegraf's influx parser default precision). */
    char ts[32];
    snprintf(ts, sizeof(ts), "%s000", values[0]);

    buf_t out = {0};
    for (int i = 0; i < g_nof_series; i++) {
        const series_t *s     = &g_series[i];
        size_t          start = out.len;
        buf_puts(&out, s->key);
        buf_puts(&out, " ");
        int nof_fields = 0;
        for (int k = 0; k < s->nof_cols; k++) {
            if (s->cols[k] < n) nof_fields += put_field(&out, nof_fields == 0, s->fields[k], values[s->cols[k]]);
        }
        if (nof_fields == 0) { /* drop a point without fields */
            out.len = start;
            out.data[start] = '\0';
            continue;
        }
        buf_puts(&out, " ");
        buf_puts(&out, ts);
        buf_puts(&out, "\n");
    }
    free(values);
    free(copy);

    int rc = 0;
    if (out.len > 0)
        rc = g_transport == TRANSPORT_UDP ? send_udp(out.data, out.len) : send_stream(out.data, out.len);
    free(out.data);

    if (rc == 0) {
        g_rows_sent++;
        g_reported_unreachable = 0;
    } else {
        g_rows_dropped++;
        if (!g_reported_unreachable) {
            fprintf(stderr, "aurora_reader: Telegraf not reachable, dropping InfluxDB rows until it is\n");
            g_reported_unreachable = 1;
        }
    }
    return rc;
}

unsigned long aurora_influx_rows_sent(void) { return g_rows_sent; }
unsigned long aurora_influx_rows_dropped(void) { return g_rows_dropped; }

void aurora_influx_close(void)
{
    if (g_fd >= 0) close(g_fd);
    g_fd = -1;
    for (int i = 0; i < g_nof_series; i++) {
        for (int k = 0; k < g_series[i].nof_cols; k++) free(g_series[i].fields[k]);
        free(g_series[i].fields);
        free(g_series[i].cols);
        free(g_series[i].key);
    }
    free(g_series);
    g_series     = NULL;
    g_nof_series = 0;
}
