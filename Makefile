CC = gcc
CFLAGS = -Wall -Wextra -Wpedantic -Werror -g -std=c11 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -pthread -lconfig -lm
INCLUDES = -I./include

SRCS = src/threeds.c src/unixds.c src/inetds2.c src/geo_processor.c src/proto.c src/logging.c
OBJS = $(SRCS:.c=.o)
TARGET = serverds

all: $(TARGET) inetclient

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS)

inetclient: clients/inetclient.c
	$(CC) $(CFLAGS) $(INCLUDES) -o clients/inetclient clients/inetclient.c src/proto.c src/geo_processor.c src/logging.c $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET) clients/inetclient
	rm -f processing/uploads/*

.PHONY: all clean