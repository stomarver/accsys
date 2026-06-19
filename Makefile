# Makefile для accsys
# Бинарники: generator, manager, manager_cli, client, client_cli, server

CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -Wpedantic -std=c99 -D_POSIX_C_SOURCE=200809L
BINS    := generator manager manager_cli client client_cli server

.PHONY: all clean debug

all: $(BINS)

# Универсальное правило: бинарник X собирается из X.c
%: %.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(BINS) *.o

# Отладочная сборка с AddressSanitizer / UBSan
debug: CFLAGS := -g -O0 -Wall -Wextra -Wpedantic -std=c99 -D_POSIX_C_SOURCE=200809L -fsanitize=address,undefined
debug: clean all
