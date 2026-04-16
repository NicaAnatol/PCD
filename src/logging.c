#include "../include/logging.h"
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>

static int log_fd = -1;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

void log_init(const char *filename) {
    if (log_fd >= 0) close(log_fd);
    log_fd = open(filename, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) log_fd = STDOUT_FILENO;
}

void log_message(const char *msg) {
    if (log_fd < 0) log_fd = STDOUT_FILENO;
    
    pthread_mutex_lock(&log_mutex);
    
    time_t now = time(NULL);
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "[%Y-%m-%d %H:%M:%S] ", localtime(&now));
    
    write(log_fd, timebuf, strlen(timebuf));
    write(log_fd, msg, strlen(msg));
    write(log_fd, "\n", 1);
    
    pthread_mutex_unlock(&log_mutex);
}

void log_int(const char *prefix, int value) {
    char buf[256];
    int len = snprintf(buf, sizeof(buf), "%s %d", prefix, value);
    if (len > 0 && len < (int)sizeof(buf)) {
        log_message(buf);
    }
}

void log_double(const char *prefix, double value) {
    char buf[256];
    int len = snprintf(buf, sizeof(buf), "%s %.6f", prefix, value);
    if (len > 0 && len < (int)sizeof(buf)) {
        log_message(buf);
    }
}
