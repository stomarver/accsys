/* plugins/msg.c */
#include <stdio.h>
#include <string.h>

static __attribute__((unused)) int msg_client_keypress(plugin_client_ctx_t *ctx, int ch, char *input_buf) {
    if ((ch == '\n' || ch == '\r') && input_buf && input_buf[0]) {
        if (strncmp(input_buf, "msg ", 4) == 0 || strcmp(input_buf, "msg") == 0) {
            const char *msg_text = "";
            if (strncmp(input_buf, "msg ", 4) == 0) {
                msg_text = input_buf + 4;
            }
            char net_pkt[512];
            snprintf(net_pkt, sizeof(net_pkt), "msg %s\n", msg_text);
            if (ctx && ctx->send_server) {
                if (ctx->send_server(net_pkt) == 0) {
                    char self_log[256];
                    snprintf(self_log, sizeof(self_log), "[%.32s] %.200s", ctx->client_name, msg_text);
                    if (ctx->add_chat_log) {
                        ctx->add_chat_log(self_log);
                    }
                }
            }
            input_buf[0] = '\0';
            return 1;
        }
    }
    return 0;
}

static __attribute__((unused)) int msg_client_packet(plugin_client_ctx_t *ctx, const char *pkt) {
    if (pkt && pkt[0] == '[') {
        if (ctx && ctx->add_chat_log) {
            ctx->add_chat_log(pkt);
        }
        return 1;
    }
    return 0;
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
