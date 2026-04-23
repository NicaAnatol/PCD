#include "../include/config.h"
#include "../include/logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static void trim_config(char *str) {
    char *start = str;
    char *end;
    
    if (!str) return;
    
    while (*start == ' ' || *start == '\t') start++;
    if (*start == '\0') {
        *str = '\0';
        return;
    }
    
    end = start + strlen(start) - 1;
    while (end > start && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) end--;
    *(end + 1) = '\0';
    
    if (start != str) memmove(str, start, strlen(start) + 1);
}

static int read_config_line(int fd, char *buf, size_t size) {
    size_t i = 0;
    char c;
    
    while (i < size - 1) {
        if (read(fd, &c, 1) <= 0) return -1;
        if (c == '\n') break;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

int load_config(const char *filename, server_config_t *config) {
    int fd;
    char line[512];
    int in_geo = 0;
    int in_bbox = 0;

    config->inet_port = 18081;
    strcpy(config->unix_socket, "/tmp/geods.sock");
    config->max_clients = 10;
    config->buffer_size = 4096;
    config->admin_timeout = 60;
    config->max_history = 100;
    config->max_points = 100000;
    config->max_segments = 1000;
    
    config->geo.earth_radius = 6371.0;
    config->geo.num_children = 4;
    strcpy(config->geo.temp_dir, "/tmp/geo_processing");
    strcpy(config->geo.uploads_dir, "processing/uploads");
    strcpy(config->geo.processing_dir, "processing/processing");
    strcpy(config->geo.outgoing_dir, "processing/outgoing");
    config->geo.bbox.min_lat = -90.0;
    config->geo.bbox.max_lat = 90.0;
    config->geo.bbox.min_lon = -180.0;
    config->geo.bbox.max_lon = 180.0;
    
    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        log_message("[CONFIG] Cannot open config file, using defaults");
        return -1;
    }
    
    while (read_config_line(fd, line, sizeof(line)) >= 0) {
        char *p = line;
        char *eq;
        
        trim_config(p);
        if (*p == '\0') continue;
        if (*p == '#') continue;
        
        if (strstr(p, "inet_port") != NULL) {
            eq = strchr(p, '=');
            if (eq) config->inet_port = atoi(eq + 1);
        }
        else if (strstr(p, "unix_socket") != NULL) {
            eq = strchr(p, '=');
            if (eq) {
                char *val = eq + 1;
                char *end;
                trim_config(val);
                end = strchr(val, ';');
                if (end) *end = '\0';
                strncpy(config->unix_socket, val, sizeof(config->unix_socket) - 1);
                config->unix_socket[sizeof(config->unix_socket) - 1] = '\0';
            }
        }
        else if (strstr(p, "max_clients") != NULL) {
            eq = strchr(p, '=');
            if (eq) config->max_clients = atoi(eq + 1);
        }
        else if (strstr(p, "buffer_size") != NULL) {
            eq = strchr(p, '=');
            if (eq) config->buffer_size = atoi(eq + 1);
        }
        else if (strstr(p, "admin_timeout") != NULL) {
            eq = strchr(p, '=');
            if (eq) config->admin_timeout = atoi(eq + 1);
        }
        else if (strstr(p, "max_history") != NULL) {
            eq = strchr(p, '=');
            if (eq) config->max_history = atoi(eq + 1);
        }
        else if (strstr(p, "max_points") != NULL) {
            eq = strchr(p, '=');
            if (eq) config->max_points = atoi(eq + 1);
        }
        else if (strstr(p, "max_segments") != NULL) {
            eq = strchr(p, '=');
            if (eq) config->max_segments = atoi(eq + 1);
        }
        else if (strstr(p, "geo:") != NULL) {
            in_geo = 1;
        }
        else if (strstr(p, "bbox:") != NULL && in_geo) {
            in_bbox = 1;
        }
        else if (in_geo && !in_bbox && strstr(p, "earth_radius") != NULL) {
            eq = strchr(p, '=');
            if (eq) config->geo.earth_radius = atof(eq + 1);
        }
        else if (in_geo && !in_bbox && strstr(p, "num_children") != NULL) {
            eq = strchr(p, '=');
            if (eq) config->geo.num_children = atoi(eq + 1);
        }
        else if (in_geo && !in_bbox && strstr(p, "temp_dir") != NULL) {
            eq = strchr(p, '=');
            if (eq) {
                char *val = eq + 1;
                char *end;
                trim_config(val);
                end = strchr(val, ';');
                if (end) *end = '\0';
                strncpy(config->geo.temp_dir, val, sizeof(config->geo.temp_dir) - 1);
                config->geo.temp_dir[sizeof(config->geo.temp_dir) - 1] = '\0';
            }
        }
        else if (in_geo && !in_bbox && strstr(p, "uploads_dir") != NULL) {
            eq = strchr(p, '=');
            if (eq) {
                char *val = eq + 1;
                char *end;
                trim_config(val);
                end = strchr(val, ';');
                if (end) *end = '\0';
                strncpy(config->geo.uploads_dir, val, sizeof(config->geo.uploads_dir) - 1);
                config->geo.uploads_dir[sizeof(config->geo.uploads_dir) - 1] = '\0';
            }
        }
        else if (in_geo && !in_bbox && strstr(p, "processing_dir") != NULL) {
            eq = strchr(p, '=');
            if (eq) {
                char *val = eq + 1;
                char *end;
                trim_config(val);
                end = strchr(val, ';');
                if (end) *end = '\0';
                strncpy(config->geo.processing_dir, val, sizeof(config->geo.processing_dir) - 1);
                config->geo.processing_dir[sizeof(config->geo.processing_dir) - 1] = '\0';
            }
        }
        else if (in_geo && !in_bbox && strstr(p, "outgoing_dir") != NULL) {
            eq = strchr(p, '=');
            if (eq) {
                char *val = eq + 1;
                char *end;
                trim_config(val);
                end = strchr(val, ';');
                if (end) *end = '\0';
                strncpy(config->geo.outgoing_dir, val, sizeof(config->geo.outgoing_dir) - 1);
                config->geo.outgoing_dir[sizeof(config->geo.outgoing_dir) - 1] = '\0';
            }
        }
        else if (in_geo && in_bbox && strstr(p, "min_lat") != NULL) {
            eq = strchr(p, '=');
            if (eq) config->geo.bbox.min_lat = atof(eq + 1);
        }
        else if (in_geo && in_bbox && strstr(p, "max_lat") != NULL) {
            eq = strchr(p, '=');
            if (eq) config->geo.bbox.max_lat = atof(eq + 1);
        }
        else if (in_geo && in_bbox && strstr(p, "min_lon") != NULL) {
            eq = strchr(p, '=');
            if (eq) config->geo.bbox.min_lon = atof(eq + 1);
        }
        else if (in_geo && in_bbox && strstr(p, "max_lon") != NULL) {
            eq = strchr(p, '=');
            if (eq) config->geo.bbox.max_lon = atof(eq + 1);
        }
        
        if (strchr(p, '}') != NULL) {
            if (in_bbox) in_bbox = 0;
            else if (in_geo) in_geo = 0;
        }
    }
    
    close(fd);
    
    return 0;
}

void print_config(server_config_t *config) {
    char buf[512];
    int len;
    
    write(STDOUT_FILENO, "=== CONFIGURATIE SERVER ===\n", 28);
    
    len = snprintf(buf, sizeof(buf), "inet_port: %d\n", config->inet_port);
    write(STDOUT_FILENO, buf, len);
    
    len = snprintf(buf, sizeof(buf), "unix_socket: %s\n", config->unix_socket);
    write(STDOUT_FILENO, buf, len);
    
    len = snprintf(buf, sizeof(buf), "max_clients: %d\n", config->max_clients);
    write(STDOUT_FILENO, buf, len);
    
    len = snprintf(buf, sizeof(buf), "buffer_size: %d\n", config->buffer_size);
    write(STDOUT_FILENO, buf, len);
    
    len = snprintf(buf, sizeof(buf), "admin_timeout: %d\n", config->admin_timeout);
    write(STDOUT_FILENO, buf, len);
    
    len = snprintf(buf, sizeof(buf), "max_history: %d\n", config->max_history);
    write(STDOUT_FILENO, buf, len);
    
    len = snprintf(buf, sizeof(buf), "max_points: %d\n", config->max_points);
    write(STDOUT_FILENO, buf, len);
    
    len = snprintf(buf, sizeof(buf), "max_segments: %d\n", config->max_segments);
    write(STDOUT_FILENO, buf, len);
    
    len = snprintf(buf, sizeof(buf), "geo.earth_radius: %.2f\n", config->geo.earth_radius);
    write(STDOUT_FILENO, buf, len);
    
    len = snprintf(buf, sizeof(buf), "geo.num_children: %d\n", config->geo.num_children);
    write(STDOUT_FILENO, buf, len);
    
    len = snprintf(buf, sizeof(buf), "geo.temp_dir: %s\n", config->geo.temp_dir);
    write(STDOUT_FILENO, buf, len);
    
    len = snprintf(buf, sizeof(buf), "geo.uploads_dir: %s\n", config->geo.uploads_dir);
    write(STDOUT_FILENO, buf, len);
    
    len = snprintf(buf, sizeof(buf), "geo.processing_dir: %s\n", config->geo.processing_dir);
    write(STDOUT_FILENO, buf, len);
    
    len = snprintf(buf, sizeof(buf), "geo.outgoing_dir: %s\n", config->geo.outgoing_dir);
    write(STDOUT_FILENO, buf, len);
    
    len = snprintf(buf, sizeof(buf), "geo.bbox: [%.1f, %.1f, %.1f, %.1f]\n",
           config->geo.bbox.min_lat, config->geo.bbox.max_lat,
           config->geo.bbox.min_lon, config->geo.bbox.max_lon);
    write(STDOUT_FILENO, buf, len);
}