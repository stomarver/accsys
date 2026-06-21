/* plugins/plugin_engine.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    int term_rows;
    int term_cols;
    int client_fd;
    const char *client_name;
    void (*move_cursor)(int row, int col);
    int (*send_server)(const char *msg);
    void (*add_chat_log)(const char *text);
} plugin_client_ctx_t;

typedef struct {
    int (*broadcast_msg)(const char *msg, int exclude_fd);
} plugin_server_ctx_t;

#include "hearts.c"
#include "msg.c"

static plugin_client_ctx_t *master_client_ctx = NULL;

static __attribute__((unused)) void plugin_engine_client_init(plugin_client_ctx_t *ctx) {
    master_client_ctx = ctx;
}

static __attribute__((unused)) int plugin_engine_client_handle_keypress(int ch, int input_len, char *input_buf) {
    if (hearts_client_keypress(master_client_ctx, ch, input_len)) return 1;
    if (msg_client_keypress(master_client_ctx, ch, input_buf)) return 1;
    return 0;
}

static __attribute__((unused)) int plugin_engine_client_handle_packet(const char *pkt) {
    if (hearts_client_packet(master_client_ctx, pkt)) return 1;
    if (msg_client_packet(master_client_ctx, pkt)) return 1;
    return 0;
}

static __attribute__((unused)) void plugin_engine_client_render(void) {
    hearts_client_render(master_client_ctx);
}

static __attribute__((unused)) void plugin_engine_server_init(plugin_server_ctx_t *ctx) {
    (void)ctx;
    printf("plugin api initialized\n");
    printf("plugin msg loaded\n");
    printf("plugin hearts loaded\n");
    fflush(stdout);
}

static __attribute__((unused)) int plugin_engine_server_handle_packet(plugin_server_ctx_t *ctx, int client_fd, const char *client_name, const char *line) {
    if (msg_server_packet(ctx, client_fd, client_name, line)) return 1;
    if (hearts_server_packet(ctx, client_fd, line)) return 1;
    return 0;
}
