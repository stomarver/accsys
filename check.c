/* ===== check — unified (2026-06-18) ===== */
/* lists active (non-expired) sessions */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

#define SESSION_DIR      "sessions"
#define SESSION_ID_LEN   32
#define SESSION_TIMEOUT  1800
#define MAX_LINE         4096
#define PATH_MAX         4096

/* ---------- utils ---------- */

static void trim_nl(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
}

static int is_valid_session_id(const char *s) {
    size_t n, i;
    if (!s) return 0;
    n = strlen(s);
    if (n != SESSION_ID_LEN) return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'a' && c <= 'f') || (c >= '0' && c <= '9')) continue;
        return 0;
    }
    return 1;
}

static long read_long_file(const char *path) {
    FILE *f;
    char buf[64];
    if (!(f = fopen(path, "r"))) return -1;
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return -1; }
    fclose(f);
    trim_nl(buf);
    return atol(buf);
}

static int read_line_file(const char *path, char *buf, size_t bufsz) {
    FILE *f;
    if (!(f = fopen(path, "r"))) return 0;
    if (!fgets(buf, (int)bufsz, f)) { fclose(f); return 0; }
    fclose(f);
    trim_nl(buf);
    return 1;
}

static int path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* ---------- formatting ---------- */

static void format_duration(long secs, char *out, size_t outsz) {
    long h = secs / 3600, m = (secs % 3600) / 60, s = secs % 60;
    snprintf(out, outsz, "%02ld:%02ld:%02ld", h, m, s);
}

static void format_timestamp(long ts, char *out, size_t outsz) {
    time_t t = (time_t)ts;
    struct tm *tm = localtime(&t);
    if (tm) strftime(out, outsz, "%H:%M:%S", tm);
    else    snprintf(out, outsz, "??:??:??");
}

/* ---------- main ---------- */

int main(void) {
    DIR *d;
    struct dirent *ent;
    char path[PATH_MAX], name[MAX_LINE];
    long start, last_seen, now = (long)time(NULL);
    int active = 0, expired = 0;

    if (!path_exists(SESSION_DIR)) {
        printf("  no sessions\n");
        return 0;
    }

    d = opendir(SESSION_DIR);
    if (!d) { fprintf(stderr, "error: cannot open sessions/\n"); return 1; }

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!is_valid_session_id(ent->d_name)) continue;

        snprintf(path, sizeof(path), "%s/%s/name", SESSION_DIR, ent->d_name);
        if (!read_line_file(path, name, sizeof(name))) continue;

        snprintf(path, sizeof(path), "%s/%s/start", SESSION_DIR, ent->d_name);
        start = read_long_file(path);
        if (start < 0) continue;

        snprintf(path, sizeof(path), "%s/%s/last-seen", SESSION_DIR, ent->d_name);
        last_seen = read_long_file(path);
        if (last_seen < 0) last_seen = start;

        if (now - last_seen > SESSION_TIMEOUT) {
            expired++;
        } else {
            char dur[32], since[32];
            format_duration(now - start, dur, sizeof(dur));
            format_timestamp(start, since, sizeof(since));
            printf("  %-16s uptime: %s  (since %s)\n", name, dur, since);
            active++;
        }
    }

    closedir(d);

    if (active == 0 && expired == 0) printf("  no sessions\n");
    else {
        printf("\n  %d active", active);
        if (expired > 0) printf(", %d expired", expired);
        printf("\n");
    }

    return 0;
}