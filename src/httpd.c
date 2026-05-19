#define _POSIX_C_SOURCE 200809L
#include "../include/proto.h"
#include "../include/logging.h"
#include "../include/config.h"
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <time.h>

#define HTTP_PORT 8080
#define BUFFER_SIZE 65536

extern server_config_t g_config;
extern queue_task_t *completed_head;
extern pthread_mutex_t completed_mutex;
extern int session_validate(int session_id);
extern int session_create(const char *username);
extern int authenticate_user(const char *user, const char *pass);
extern int get_task_result(int task_id, task_result_t *result);
extern void get_task_status(int task_id, char *buf, size_t bufsize);
extern int cancel_task(int task_id);
extern int queue_add_task_file(const char *filename, const char *upload_path, int client_id, int sock_fd,
                                const char *bbox, double epsilon, int show_segments,
                                int dist_idx1, int dist_idx2, int request_id);
extern void stats_add_processed(int points, double distance, const char *filename);
extern void add_to_history(const char *command);

static int extract_session_id(const char *headers) {
    char *token_line = strstr(headers, "X-Session-ID:");
    if (token_line) {
        token_line += 13;
        while (*token_line == ' ') token_line++;
        int session_id = atoi(token_line);
        if (session_validate(session_id)) {
            return session_id;
        }
    }
    return 0;
}

static void http_send_response(int client_fd, int status_code, const char *status_text, 
                                const char *content_type, const char *body) {
    char response[BUFFER_SIZE];
    int len = snprintf(response, sizeof(response),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        status_code, status_text, content_type, strlen(body), body);
    
    send(client_fd, response, len, 0);
}

static void http_send_unauthorized(int client_fd) {
    char response[] = "HTTP/1.1 401 Unauthorized\r\n"
                      "WWW-Authenticate: Basic realm=\"GeoServer\"\r\n"
                      "Content-Length: 0\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    send(client_fd, response, strlen(response), 0);
}

static int extract_task_id(const char *uri) {
    const char *last_slash = strrchr(uri, '/');
    if (last_slash) {
        return atoi(last_slash + 1);
    }
    return -1;
}

static void parse_upload_params(const char *uri, char *bbox, double *epsilon, 
                                  int *show_segments, int *dist_idx1, int *dist_idx2) {
    char *question = strchr(uri, '?');
    if (!question) return;
    
    char *params = strdup(question + 1);
    char *token = strtok(params, "&");
    
    while (token) {
        if (strncmp(token, "bbox=", 5) == 0) {
            strncpy(bbox, token + 5, 127);
            bbox[127] = '\0';
        } else if (strncmp(token, "epsilon=", 8) == 0) {
            *epsilon = atof(token + 8);
        } else if (strncmp(token, "segments=", 9) == 0) {
            *show_segments = atoi(token + 9);
        } else if (strncmp(token, "idx1=", 5) == 0) {
            *dist_idx1 = atoi(token + 5);
        } else if (strncmp(token, "idx2=", 5) == 0) {
            *dist_idx2 = atoi(token + 5);
        }
        token = strtok(NULL, "&");
    }
    free(params);
}

static void handle_upload(int client_fd, const char *body, const char *uri, int session_id) {
    if (session_id <= 0) {
        http_send_unauthorized(client_fd);
        return;
    }
    
    char bbox[128] = "";
    double epsilon = -1;
    int show_segments = 0;
    int dist_idx1 = 0, dist_idx2 = 0;
    
    parse_upload_params(uri, bbox, &epsilon, &show_segments, &dist_idx1, &dist_idx2);
    
    char filename[256] = "";
    char *fn_start = strstr(body, "\"filename\"");
    if (fn_start) {
        fn_start = strchr(fn_start, ':');
        if (fn_start) {
            fn_start++;
            while (*fn_start == ' ' || *fn_start == '"') fn_start++;
            char *fn_end = fn_start;
            while (*fn_end && *fn_end != '"') fn_end++;
            int len = fn_end - fn_start;
            if (len > 0 && len < 255) {
                strncpy(filename, fn_start, len);
                filename[len] = '\0';
            }
        }
    }
    
    if (filename[0] == '\0') {
        http_send_response(client_fd, 400, "Bad Request", "application/json", 
                          "{\"error\":\"Missing filename\"}");
        return;
    }
    
    char upload_path[512];
    snprintf(upload_path, sizeof(upload_path), "processing/uploads/%d_%s", session_id, filename);
    
    int task_id = queue_add_task_file(filename, upload_path, session_id, -1,
                                       bbox, epsilon, show_segments,
                                       dist_idx1, dist_idx2, 0);
    
    if (task_id > 0) {
        char response[256];
        snprintf(response, sizeof(response), "{\"task_id\":%d,\"status\":\"pending\"}", task_id);
        http_send_response(client_fd, 200, "OK", "application/json", response);
        add_to_history("HTTP Upload");
    } else {
        http_send_response(client_fd, 500, "Internal Error", "application/json", 
                          "{\"error\":\"Failed to create task\"}");
    }
}

static void handle_task_status(int client_fd, int task_id, int session_id) {
    if (session_id <= 0) {
        http_send_unauthorized(client_fd);
        return;
    }
    
    char status_buf[256];
    get_task_status(task_id, status_buf, sizeof(status_buf));
    
    char response[512];
    snprintf(response, sizeof(response), "{\"task_id\":%d,\"status\":\"%s\"}", task_id, status_buf);
    http_send_response(client_fd, 200, "OK", "application/json", response);
}

static void handle_task_result(int client_fd, int task_id, int session_id) {
    if (session_id <= 0) {
        http_send_unauthorized(client_fd);
        return;
    }
    
    task_result_t res;
    if (get_task_result(task_id, &res)) {
        char response[4096];
        snprintf(response, sizeof(response), 
                "{\"task_id\":%d,\"total_distance\":%s,\"point_count\":%s,\"segment_count\":%s}",
                task_id, res.total_distance, res.point_count, res.segment_count);
        http_send_response(client_fd, 200, "OK", "application/json", response);
    } else {
        http_send_response(client_fd, 404, "Not Found", "application/json", 
                          "{\"error\":\"Task not found or not completed\"}");
    }
}

static void handle_task_cancel(int client_fd, int task_id, int session_id) {
    if (session_id <= 0) {
        http_send_unauthorized(client_fd);
        return;
    }
    
    if (cancel_task(task_id)) {
        http_send_response(client_fd, 200, "OK", "application/json", 
                          "{\"status\":\"cancelled\"}");
    } else {
        http_send_response(client_fd, 404, "Not Found", "application/json", 
                          "{\"error\":\"Task not found or already completed\"}");
    }
}

static void handle_stats(int client_fd, int session_id) {
    if (session_id <= 0) {
        http_send_unauthorized(client_fd);
        return;
    }
    
    server_stats_t stats;
    get_stats(&stats);
    
    char response[1024];
    snprintf(response, sizeof(response),
            "{\"active_clients\":%d,\"active_processes\":%d,\"total_points\":%d,\"total_distance\":%.2f}",
            stats.active_clients, stats.active_processes, 
            stats.total_processed_points, stats.total_processed_distance);
    http_send_response(client_fd, 200, "OK", "application/json", response);
}

static void handle_login(int client_fd, const char *body) {
    char username[64] = "";
    char password[64] = "";
    
    char *user_start = strstr(body, "\"username\"");
    if (user_start) {
        user_start = strchr(user_start, ':');
        if (user_start) {
            user_start++;
            while (*user_start == ' ' || *user_start == '"') user_start++;
            char *user_end = user_start;
            while (*user_end && *user_end != '"') user_end++;
            int len = user_end - user_start;
            if (len > 0 && len < 63) {
                strncpy(username, user_start, len);
                username[len] = '\0';
            }
        }
    }
    
    char *pass_start = strstr(body, "\"password\"");
    if (pass_start) {
        pass_start = strchr(pass_start, ':');
        if (pass_start) {
            pass_start++;
            while (*pass_start == ' ' || *pass_start == '"') pass_start++;
            char *pass_end = pass_start;
            while (*pass_end && *pass_end != '"') pass_end++;
            int len = pass_end - pass_start;
            if (len > 0 && len < 63) {
                strncpy(password, pass_start, len);
                password[len] = '\0';
            }
        }
    }
    
    if (authenticate_user(username, password)) {
        int session_id = session_create(username);
        char response[256];
        snprintf(response, sizeof(response), "{\"session_id\":%d,\"status\":\"ok\"}", session_id);
        http_send_response(client_fd, 200, "OK", "application/json", response);
    } else {
        http_send_response(client_fd, 401, "Unauthorized", "application/json", 
                          "{\"error\":\"Invalid credentials\"}");
    }
}

static void parse_http_request(const char *request, char *method, char *uri, char *body, char *headers) {
    method[0] = '\0';
    uri[0] = '\0';
    body[0] = '\0';
    headers[0] = '\0';
    
    char *line = strdup(request);
    char *first_line = strtok(line, "\r\n");
    if (first_line) {
        sscanf(first_line, "%s %s", method, uri);
    }
    
    char *headers_start = strstr(request, "\r\n");
    if (headers_start) {
        headers_start += 2;
        char *headers_end = strstr(headers_start, "\r\n\r\n");
        if (headers_end) {
            int len = headers_end - headers_start;
            if (len < BUFFER_SIZE - 1) {
                strncpy(headers, headers_start, len);
                headers[len] = '\0';
            }
        }
    }
    
    char *body_start = strstr(request, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        strncpy(body, body_start, BUFFER_SIZE - 1);
    }
    
    free(line);
}

void *http_main(void *args) {
    (void)args;
    int server_fd, client_fd;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);
    char buffer[BUFFER_SIZE];
    
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        log_message("[HTTP] Socket creation failed");
        pthread_exit(NULL);
    }
    
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(HTTP_PORT);
    
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        log_message("[HTTP] Bind failed");
        close(server_fd);
        pthread_exit(NULL);
    }
    
    if (listen(server_fd, 10) < 0) {
        log_message("[HTTP] Listen failed");
        close(server_fd);
        pthread_exit(NULL);
    }
    
    char logbuf[128];
    snprintf(logbuf, sizeof(logbuf), "[HTTP] Server started on port %d", HTTP_PORT);
    log_message(logbuf);
    
    while (1) {
        client_fd = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen);
        if (client_fd < 0) continue;
        
        ssize_t bytes = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
        if (bytes > 0) {
            buffer[bytes] = '\0';
            
            char method[16], uri[256], body[BUFFER_SIZE], headers[BUFFER_SIZE];
            parse_http_request(buffer, method, uri, body, headers);
            
            int session_id = extract_session_id(headers);
            
            if (strcmp(method, "POST") == 0 && strcmp(uri, "/api/login") == 0) {
                handle_login(client_fd, body);
            }
            else if (strcmp(method, "GET") == 0 && strcmp(uri, "/api/stats") == 0) {
                handle_stats(client_fd, session_id);
            }
            else if (strcmp(method, "GET") == 0 && strstr(uri, "/api/task/") != NULL && strstr(uri, "/result") != NULL) {
                char uri_copy[256];
                strncpy(uri_copy, uri, sizeof(uri_copy) - 1);
                char *result_pos = strstr(uri_copy, "/result");
                if (result_pos) *result_pos = '\0';
                int task_id = extract_task_id(uri_copy);
                if (task_id > 0) {
                    handle_task_result(client_fd, task_id, session_id);
                } else {
                    http_send_response(client_fd, 400, "Bad Request", "application/json", 
                                      "{\"error\":\"Invalid task ID\"}");
                }
            }
            else if (strcmp(method, "GET") == 0 && strncmp(uri, "/api/task/", 10) == 0) {
                int task_id = extract_task_id(uri);
                if (task_id > 0) {
                    handle_task_status(client_fd, task_id, session_id);
                } else {
                    http_send_response(client_fd, 400, "Bad Request", "application/json", 
                                      "{\"error\":\"Invalid task ID\"}");
                }
            }
            else if (strcmp(method, "DELETE") == 0 && strncmp(uri, "/api/task/", 10) == 0) {
                int task_id = extract_task_id(uri);
                if (task_id > 0) {
                    handle_task_cancel(client_fd, task_id, session_id);
                } else {
                    http_send_response(client_fd, 400, "Bad Request", "application/json", 
                                      "{\"error\":\"Invalid task ID\"}");
                }
            }
            else if (strcmp(method, "POST") == 0 && strncmp(uri, "/api/upload", 11) == 0) {
                handle_upload(client_fd, body, uri, session_id);
            }
            else {
                http_send_response(client_fd, 404, "Not Found", "application/json", 
                                  "{\"error\":\"Endpoint not found\"}");
            }
        }
        
        close(client_fd);
    }
    
    close(server_fd);
    return NULL;
}