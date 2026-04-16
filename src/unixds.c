
#include "../include/proto.h"
#include "../include/logging.h"
#include <sys/time.h>

extern void get_stats(server_stats_t *stats);
extern void stats_increment_processes(void);
extern void stats_decrement_processes(void);
extern void format_history_response(char *buffer, size_t bufsize);
extern void format_queue_response(char *buffer, size_t bufsize);
extern void format_avg_time_response(char *buffer, size_t bufsize);

void format_stats_response(server_stats_t *stats, char *buffer, size_t bufsize) {
    (void)bufsize;
    snprintf(buffer, 1024,
             "=== STATISTICI SERVER ===\n"
             "Clienti activi: %d\n"
             "Procese active: %d\n"
             "Total puncte procesate: %d\n"
             "Distanta totala: %.2f km\n"
             "Ultimul upload: %s\n"
             "========================\n",
             stats->active_clients,
             stats->active_processes,
             stats->total_processed_points,
             stats->total_processed_distance,
             stats->last_upload);
}

void format_clients_response(char *buffer, size_t bufsize) {
    (void)bufsize;
    server_stats_t stats;
    get_stats(&stats);
    snprintf(buffer, 512,
             "Clienti activi: %d\n"
             "Procese active: %d\n"
             "Socket UNIX: /tmp/geods\n"
             "Port INET: 18081\n",
             stats.active_clients, stats.active_processes);
}

void *unix_main(void *args) {
    char *socket_path = (char *)args;
    int sock, client_fd;
    struct sockaddr_un addr;
    socklen_t addr_len = sizeof(addr);
    fd_set read_fds;
    char buffer[4096];
    ssize_t bytes;
    int admin_connected = 0;
    struct timeval tv;
    
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        log_message("[UNIX] socket creation failed");
        pthread_exit(NULL);
    }
    
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';
    
    unlink(socket_path);
    
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_message("[UNIX] bind failed");
        pthread_exit(NULL);
    }
    
    if (listen(sock, 5) < 0) {
        log_message("[UNIX] listen failed");
        pthread_exit(NULL);
    }
    
    char logbuf[256];
    snprintf(logbuf, sizeof(logbuf), "[UNIX] Server listening on %s", socket_path);
    log_message(logbuf);
    
    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(sock, &read_fds);
        
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        if (select(sock + 1, &read_fds, NULL, NULL, &tv) < 0) {
            continue;
        }
        
        if (FD_ISSET(sock, &read_fds)) {
            if (admin_connected) {
                client_fd = accept(sock, (struct sockaddr *)&addr, &addr_len);
                if (client_fd >= 0) {
                    char msg[] = "Eroare: Un admin este deja conectat!\n";
                    write(client_fd, msg, sizeof(msg)-1);
                    close(client_fd);
                }
                continue;
            }
            
            client_fd = accept(sock, (struct sockaddr *)&addr, &addr_len);
            if (client_fd < 0) continue;
            
            admin_connected = 1;
            log_message("[UNIX] Admin connected");
            
            struct timeval timeout;
            timeout.tv_sec = 60;
            timeout.tv_usec = 0;
            setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            
            while (admin_connected) {
                bytes = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
                
                if (bytes <= 0) {
                    break;
                }
                
                buffer[bytes] = '\0';
                if (buffer[bytes - 1] == '\n') buffer[bytes - 1] = '\0';
                
                if (strcmp(buffer, "STATS") == 0) {
                    server_stats_t stats;
                    get_stats(&stats);
                    format_stats_response(&stats, buffer, sizeof(buffer));
                    write(client_fd, buffer, strlen(buffer));
                }
                else if (strcmp(buffer, "CLIENTS") == 0) {
                    format_clients_response(buffer, sizeof(buffer));
                    write(client_fd, buffer, strlen(buffer));
                }
                else if (strcmp(buffer, "HISTORY") == 0) {
                    format_history_response(buffer, sizeof(buffer));
                    write(client_fd, buffer, strlen(buffer));
                }
                else if (strcmp(buffer, "QUEUE") == 0) {
                    format_queue_response(buffer, sizeof(buffer));
                    write(client_fd, buffer, strlen(buffer));
                }
                else if (strcmp(buffer, "AVG_TIME") == 0) {
                    format_avg_time_response(buffer, sizeof(buffer));
                    write(client_fd, buffer, strlen(buffer));
                }
                else if (strcmp(buffer, "PROCESSES") == 0) {
                    pid_t pid = fork();
                    if (pid == 0) {
                        stats_increment_processes();
                        sleep(5);
                        stats_decrement_processes();
                        _exit(0);
                    }
                    char msg[] = "Proces creat (fork test)\n";
                    write(client_fd, msg, sizeof(msg)-1);
                }
                else if (strcmp(buffer, "PING") == 0) {
                    write(client_fd, "PONG", 4);
                }
                else if (strcmp(buffer, "EXIT") == 0) {
                    break;
                }
                else {
                    char msg[] = "Comenzi: STATS, CLIENTS, HISTORY, QUEUE, AVG_TIME, PROCESSES, EXIT\n";
                    write(client_fd, msg, sizeof(msg)-1);
                }
            }
            
            close(client_fd);
            admin_connected = 0;
            log_message("[UNIX] Admin disconnected");
        }
    }
    
    close(sock);
    unlink(socket_path);
    pthread_exit(NULL);
}
