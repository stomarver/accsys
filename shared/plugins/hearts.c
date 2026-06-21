/* plugins/hearts.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int wandering_heart_timer = 0;
static int wandering_heart_row = 5;
static int wandering_heart_col = 10;

static __attribute__((unused)) void trigger_wandering_heart_action(plugin_client_ctx_t *ctx) {
    if (!ctx) return;
    wandering_heart_timer = 5 * 20; /* 5 seconds at 20 FPS */
    if (ctx->term_rows > 7) {
        wandering_heart_row = 5 + rand() % (ctx->term_rows - 7);
    } else {
        wandering_heart_row = 5;
    }
    if (ctx->term_cols > 5) {
        wandering_heart_col = 2 + rand() % (ctx->term_cols - 4);
    } else {
        wandering_heart_col = 2;
    }
}

static __attribute__((unused)) int hearts_client_keypress(plugin_client_ctx_t *ctx, int ch, int input_len) {
    if (ch == ' ' && input_len == 0) {
        trigger_wandering_heart_action(ctx);
        if (ctx && ctx->send_server) {
            ctx->send_server("heart\n");
        }
        return 1;
    }
    return 0;
}

static __attribute__((unused)) int hearts_client_packet(plugin_client_ctx_t *ctx, const char *pkt) {
    if (pkt && (strcmp(pkt, "heart") == 0 || strstr(pkt, "♥") != NULL)) {
        trigger_wandering_heart_action(ctx);
        if (strcmp(pkt, "heart") == 0) return 1;
    }
    return 0;
}

static __attribute__((unused)) void hearts_client_render(plugin_client_ctx_t *ctx) {
    if (wandering_heart_timer > 0) {
        wandering_heart_timer--;
        if (ctx && ctx->move_cursor) {
            ctx->move_cursor(wandering_heart_row, wandering_heart_col);
            fputs("\033[91m♥\033[0m", stdout);
        }
    }
}

static __attribute__((unused)) int hearts_server_packet(plugin_server_ctx_t *ctx, int client_fd, const char *line) {
    if (strcmp(line, "heart") == 0 || strcmp(line, "HEART") == 0) {
        if (ctx && ctx->broadcast_msg) {
            ctx->broadcast_msg("heart\n", client_fd);
        }
        return 1;
    }
    return 0;
}
