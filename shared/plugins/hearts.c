/* plugins/hearts.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int wandering_heart_timer = 0;
static int wandering_heart_row = 0;
static int wandering_heart_col = 0;

static __attribute__((unused)) void trigger_wandering_heart_action(plugin_client_ctx_t *ctx, int row, int col, int ttl_seconds) {
    (void)ctx;
    wandering_heart_row = row;
    wandering_heart_col = col;
    if (ttl_seconds <= 0) ttl_seconds = 5;
    wandering_heart_timer = ttl_seconds * 20; /* 20 FPS */
}

/* client keypress – Ctrl+T only */
static __attribute__((unused)) int hearts_client_keypress(plugin_client_ctx_t *ctx, int ch, int input_len) {
    if (ch == 20 && input_len == 0) { /* Ctrl+T */
        if (ctx && ctx->send_server) {
            ctx->send_server("heart\n");
        }
        return 1;
    }
    return 0;
}

/* packet: "heart <row> <col>" | "heart <row> <col> <ttl>" | "heart_off" */
static __attribute__((unused)) int hearts_client_packet(plugin_client_ctx_t *ctx, const char *pkt) {
    if (!pkt) return 0;
    if (strncmp(pkt, "heart_off", 9) == 0) {
        wandering_heart_timer = 0;
        return 1;
    }
    if (strncmp(pkt, "heart ", 6) == 0) {
        int r=0,c=0,ttl=5;
        if (sscanf(pkt+6, "%d %d %d", &r, &c, &ttl) >= 2) {
            trigger_wandering_heart_action(ctx, r, c, ttl);
            return 1;
        }
    }
    if (strcmp(pkt, "heart") == 0) {
        /* legacy fallback */
        trigger_wandering_heart_action(ctx, rand()%10, rand()%30, 5);
        return 1;
    }
    return 0;
}

static __attribute__((unused)) void hearts_client_render(plugin_client_ctx_t *ctx) {
    if (wandering_heart_timer > 0 && ctx) {
        wandering_heart_timer--;
        int row0 = ctx->chat_row0 > 0 ? ctx->chat_row0 : 5;
        int col0 = ctx->chat_col0 > 0 ? ctx->chat_col0 : 3;
        int h = ctx->chat_height > 0 ? ctx->chat_height : 10;
        int w = ctx->chat_width > 0 ? ctx->chat_width : 60;
        int r = wandering_heart_row;
        int c = wandering_heart_col;
        if (r < 0) r = 0;
        if (r >= h) r = h-1;
        if (c < 0) c = 0;
        if (c >= w) c = w-1;
        if (ctx->move_cursor) {
            ctx->move_cursor(row0 + r, col0 + c);
            fputs("\033[91m♥\033[0m", stdout);
        }
    }
}

/* server side – authoritative heart */
static int srv_heart_active = 0;
static time_t srv_heart_expires = 0;
static int srv_heart_row = 0;
static int srv_heart_col = 0;

static __attribute__((unused)) int hearts_server_packet(plugin_server_ctx_t *ctx, int client_fd, const char *line) {
    time_t now = time(NULL);
    if (srv_heart_active && now >= srv_heart_expires) {
        srv_heart_active = 0;
    }
    if (strcmp(line, "heart") == 0) {
        srv_heart_active = 1;
        srv_heart_expires = now + 5;
        srv_heart_row = rand() % 10;
        srv_heart_col = rand() % 40;
        char out[64];
        snprintf(out, sizeof(out), "heart %d %d 5\n", srv_heart_row, srv_heart_col);
        if (ctx && ctx->broadcast_msg) {
            ctx->broadcast_msg(out, -1);
        }
        return 1;
    }
    if (strcmp(line, "heart_query") == 0) {
        char out[64];
        if (srv_heart_active && now < srv_heart_expires) {
            long ttl = (long)(srv_heart_expires - now);
            if (ttl < 1) ttl = 1;
            snprintf(out, sizeof(out), "heart %d %d %ld\n", srv_heart_row, srv_heart_col, ttl);
        } else {
            srv_heart_active = 0;
            snprintf(out, sizeof(out), "heart_off\n");
        }
        write(client_fd, out, strlen(out));
        return 1;
    }
    return 0;
}
