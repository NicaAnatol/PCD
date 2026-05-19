CC = gcc
CFLAGS = -Wall -Wextra -Wpedantic -Werror -g -std=c11 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -pthread -lm -lgeos_c
INCLUDES = -I./include

SRCS = src/threeds.c src/unixds.c src/inetds2.c src/geo_processor.c src/proto.c src/logging.c src/file_io.c src/server_common.c src/config.c src/httpd.c
TARGET = serverds

CLIENT_SRCS = clients/inetclient.c src/geo_processor.c src/logging.c src/file_io.c
CLIENT = clients/inetclient

ADMIN_SRCS = clients/admin/admin_client.c
ADMIN = clients/admin/admin_client

all: $(TARGET) $(CLIENT) $(ADMIN)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS)

$(CLIENT): $(CLIENT_SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS)

$(ADMIN): $(ADMIN_SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(LDFLAGS) -lncurses

clean:
	rm -f $(TARGET) $(CLIENT) $(ADMIN)
	rm -f processing/uploads/*
	rm -f /tmp/geods
	rm -f /tmp/geods_client_*

valgrind-server:
	valgrind --leak-check=full --track-origins=yes --show-leak-kinds=all --log-file=valgrind_server.log ./serverds

valgrind-client:
	valgrind --leak-check=full --track-origins=yes --show-leak-kinds=all --log-file=valgrind_client.log ./clients/inetclient 127.0.0.1 18081

valgrind-both:
	valgrind --leak-check=full --track-origins=yes --show-leak-kinds=all --log-file=valgrind_server.log ./serverds &
	sleep 2
	valgrind --leak-check=full --track-origins=yes --show-leak-kinds=all --log-file=valgrind_client.log ./clients/inetclient 127.0.0.1 18081
	-killall serverds

valgrind-quick:
	valgrind --leak-check=summary --track-origins=yes ./clients/inetclient 127.0.0.1 18081

.PHONY: all clean valgrind-server valgrind-client valgrind-both valgrind-quick