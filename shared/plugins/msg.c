/* plugins/msg.c */
#include <stdio.h>
#include <string.h>

/* msg plugin – chat history with scroll */
#ifndef MSG_HISTORY_MAX
#define MSG_HISTORY_MAX 1024
#endif

static char msg_history[MSG_HISTORY_MAX][256];
static int  msg_history_count = 0;
static int  msg_history_scroll = 0; /* 0 = bottom, >0 = lines scrolled up */

static int msg_history_visible_lines(plugin_client_ctx_t *ctx) {
    if (ctx && ctx->chat_height > 0) return ctx->chat_height;
    return 10;
}

static __attribute__((unused)) int msg_client_keypress(plugin_client_ctx_t *ctx, int ch, char *input_buf) {
    /* arrow up/down for history scroll – only when input empty */
    if ((!input_buf || !*input_buf)) {
        if (ch == 27) { /* ESC sequence handled by caller? we get raw ch, so look for arrow via plugin_engine wrapper – actually client passes raw, so handle ESC [ A/B in client, here just return 0 */ }
    }
    if (input_buf && input_buf[0]) {
        if (ch == '\n' || ch == '\r') {
            if (strncmp(input_buf, "msg ", 4) == 0 || strcmp(input_buf, "msg") == 0) {
                const char *msg_text = "";
                if (strncmp(input_buf, "msg ", 4) == 0) msg_text = input_buf + 4;
                char net_pkt[512];
                snprintf(net_pkt, sizeof(net_pkt), "msg %s\n", msg_text);
                if (ctx && ctx->send_server) {
                    if (ctx->send_server(net_pkt) == 0) {
                        char self_log[256];
                        snprintf(self_log, sizeof(self_log), "[%.32s] %.200s", ctx->client_name ? ctx->client_name : "me", msg_text);
                        if (ctx->add_chat_log) ctx->add_chat_log(self_log);
                    }
                }
                input_buf[0] = '\0';
                return 1;
            }
        }
    }
    return 0;
}

/* returns 1 if handled arrow scroll */
static __attribute__((unused)) int msg_client_handle_arrow(plugin_client_ctx_t *ctx, int arrow) {
    int vis = msg_history_visible_lines(ctx);
    int max_scroll = msg_history_count - vis;
    if (max_scroll < 0) max_scroll = 0;
    if (arrow == 'A') { /* up */
        if (msg_history_scroll < max_scroll) msg_history_scroll++;
        return 1;
    }
    if (arrow == 'B') { /* down */
        if (msg_history_scroll > 0) msg_history_scroll--;
        return 1;
    }
    return 0;
}

static __attribute__((unused)) int msg_client_packet(plugin_client_ctx_t *ctx, const char *pkt) {
    (void)ctx;
    if (pkt && pkt[0] == '[') {
        /* add to ring */
        if (msg_history_count < MSG_HISTORY_MAX) {
            strncpy(msg_history[msg_history_count], pkt, 255);
            msg_history[msg_history_count][255] = '\0';
            msg_history_count++;
        } else {
            /* shift */
            memmove(msg_history[0], msg_history[1], (MSG_HISTORY_MAX-1)*256);
            strncpy(msg_history[MSG_HISTORY_MAX-1], pkt, 255);
            msg_history[MSG_HISTORY_MAX-1][255]='\0';
        }
        /* auto-scroll to bottom */
        msg_history_scroll = 0;
        if (ctx && ctx->add_chat_log) ctx->add_chat_log(pkt);
        return 1;
    }
    return 0;
}

static __attribute__((unused)) void msg_client_get_visible(int *start_out, int *count_out, plugin_client_ctx_t *ctx) {
    int vis = msg_history_visible_lines(ctx);
    int total = msg_history_count;
    if (total < vis) vis = total;
    int start = total - vis - msg_history_scroll;
    if (start < 0) start = 0;
    if (start_out) *start_out = start;
    if (count_out) *count_out = vis;
}

static __attribute__((unused)) const char* msg_client_get_line(int idx) {
    if (idx < 0 || idx >= msg_history_count) return NULL;
    if (idx >= MSG_HISTORY_MAX) return NULL;
    return msg_history[idx];
}

static __attribute__((unused)) int msg_server_packet(plugin_server_ctx_t *ctx, int client_fd, const char *client_name, const char *line) {
    if (strncmp(line, "msg ", 4) == 0) {
        char chat_buf[512];
        snprintf(chat_buf, sizeof(chat_buf), "[%s] %s\n", client_name, line + 4);
        printf("%s", chat_buf);
        fflush(stdout);
        if (ctx && ctx->broadcast_msg) {
            ctx->broadcast_msg(chat_buf, client_fd);
        }
        return 1;
    }
    return 0;
}
