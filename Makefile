# Makefile для accsys
# Бинарники: shared/generator, shared/manager, shared/manager_cli, client/client, client/client_cli, server/server

CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -Wpedantic -std=c99 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE
BINS    := shared/generator shared/manager shared/manager_cli client/client client/client_cli server/server shared/plugins/hearts

.PHONY: all clean debug

all: $(BINS)

shared/generator: shared/generator.c
	$(CC) $(CFLAGS) -o $@ $<
	chmod +x $@

shared/manager: shared/manager.c
	$(CC) $(CFLAGS) -o $@ $<
	chmod +x $@

shared/manager_cli: shared/manager_cli.c
	$(CC) $(CFLAGS) -o $@ $<
	chmod +x $@

client/client: client/client.c
	$(CC) $(CFLAGS) -o $@ $<
	chmod +x $@

client/client_cli: client/client_cli.c
	$(CC) $(CFLAGS) -o $@ $<
	chmod +x $@

server/server: server/server.c
	$(CC) $(CFLAGS) -o $@ $<
	chmod +x $@

# hearts plugin – standalone binary for Ctrl+T
shared/plugins/hearts: shared/plugins/hearts_cli.c
	$(CC) $(CFLAGS) -o $@ $<
	chmod +x $@

clean:
	rm -f $(BINS) *.o

debug: CFLAGS := -g -O0 -Wall -Wextra -Wpedantic -std=c99 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -fsanitize=address,undefined
debug: clean all
