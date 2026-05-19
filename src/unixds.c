#define _POSIX_C_SOURCE 200809L
#include "../include/proto.h"      // Pentru structuri, constante si functii comune ale serverului
#include "../include/logging.h"    // Pentru logarea evenimentelor si erorilor
#include <../include/config.h>     // Pentru configuratia globala a serverului
#include <sys/time.h>              // Pentru struct timeval folosit la select() si timeout-uri
extern pthread_barrier_t startup_barrier;
// Declaratii externe pentru functiile implementate in alte module.
// Acestea sunt folosite aici pentru statistici, istoric, sesiuni si administrare.
extern void get_stats(server_stats_t *stats);
extern void stats_increment_processes(void);
extern void stats_decrement_processes(void);
extern void format_history_response(char *buffer, size_t bufsize);
extern void format_queue_response(char *buffer, size_t bufsize);
extern void format_avg_time_response(char *buffer, size_t bufsize);
extern void format_sessions_response(char *buffer, size_t bufsize);
extern int terminate_session(int session_id);
extern void blacklist_add(const char *ip);
extern void blacklist_remove(const char *ip);
extern int cancel_task(int task_id);
extern int force_disconnect_client(int session_id);
// Formateaza intr-un buffer text statisticile generale ale serverului.
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

// Formateaza un raspuns scurt despre clientii si resursele active ale serverului.
void format_clients_response(char *buffer, size_t bufsize) {
    (void)bufsize;
    server_stats_t stats;
    get_stats(&stats);
    
    snprintf(buffer, 512,
         "Clienti activi: %d\n"
         "Procese active: %d\n"
         "Socket UNIX: %s\n"
         "Port INET: %d\n",
         stats.active_clients, stats.active_processes,
         g_config.unix_socket, g_config.inet_port);
}

// Thread-ul principal pentru interfata administrativa pe socket UNIX.
// Permite conectarea unui singur admin si interpretarea comenzilor text primite de la acesta.
void *unix_main(void *args) {
    pthread_barrier_wait(&startup_barrier);
    char *socket_path = (char *)args;
    int sock;
    struct sockaddr_un addr, client_addr;
    socklen_t client_addr_len;
    char buffer[65536];
    ssize_t bytes;
    struct timeval tv;
    fd_set read_fds;
    
    // Creeaza socket-ul local de tip UNIX DGRAM (neconectat)
    sock = socket(AF_UNIX, SOCK_DGRAM, 0);
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
        char errbuf[256];
        snprintf(errbuf, sizeof(errbuf), "[UNIX] bind failed: %s", strerror(errno));
        log_message(errbuf);
        pthread_exit(NULL);
    }
    
    char logbuf[512];
    snprintf(logbuf, sizeof(logbuf), "[UNIX] DGRAM socket bound to path: '%s'", addr.sun_path);
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
            client_addr_len = sizeof(client_addr);
            bytes = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
                            (struct sockaddr *)&client_addr, &client_addr_len);
            
            if (bytes <= 0) continue;
            
            buffer[bytes] = '\0';
            if (buffer[bytes - 1] == '\n') buffer[bytes - 1] = '\0';
            
            char response[65536];
            response[0] = '\0';
            char debug_msg[68000];
snprintf(debug_msg, sizeof(debug_msg), "[UNIX] Received command: '%s'", buffer);
log_message(debug_msg);
            // Procesare comenzi (la fel ca înainte)
            if (strcmp(buffer, "STATS") == 0) {
                server_stats_t stats;
                get_stats(&stats);
                format_stats_response(&stats, response, sizeof(response));
            }
            else if (strcmp(buffer, "CLIENTS") == 0) {
                format_clients_response(response, sizeof(response));
            }
            else if (strcmp(buffer, "HISTORY") == 0) {
                format_history_response(response, sizeof(response));
            }
            else if (strcmp(buffer, "QUEUE") == 0) {
                format_queue_response(response, sizeof(response));
            }
            else if (strcmp(buffer, "AVG_TIME") == 0) {
                format_avg_time_response(response, sizeof(response));
            }
            else if (strcmp(buffer, "SESSIONS") == 0) {
                format_sessions_response(response, sizeof(response));
            }
            else if (strcmp(buffer, "PROCESSES") == 0) {
                pid_t pid = fork();
                if (pid == 0) {
                    stats_increment_processes();
                    sleep(5);
                    stats_decrement_processes();
                    _exit(0);
                }
                snprintf(response, sizeof(response), "Proces creat (fork test)\n");
            }
            else if (strncmp(buffer, "KILL", 4) == 0) {
                int session_id = atoi(buffer + 5);
                if (terminate_session(session_id)) {
                    snprintf(response, sizeof(response), "Sesiune terminata\n");
                } else {
                    snprintf(response, sizeof(response), "Sesiune negasita\n");
                }
            }
            else if (strncmp(buffer, "BLOCK_IP", 8) == 0) {
                char *ip = buffer + 9;
                blacklist_add(ip);
                snprintf(response, sizeof(response), "IP blocked.\n");
            }
            else if (strncmp(buffer, "UNBLOCK_IP", 10) == 0) {
                char *ip = buffer + 11;
                blacklist_remove(ip);
                snprintf(response, sizeof(response), "IP unblocked.\n");
            }
            else if (strncmp(buffer, "CANCEL", 6) == 0) {
                int task_id = atoi(buffer + 7);
                if (cancel_task(task_id))
                    snprintf(response, sizeof(response), "Task cancelled.\n");
                else
                    snprintf(response, sizeof(response), "Task not found or already done.\n");
            }
            else if (strncmp(buffer, "BLOCK_DOMAIN", 12) == 0) {
                char *domain = buffer + 13;
                domain_blacklist_add(domain);
                snprintf(response, sizeof(response), "Domain blocked.\n");
            }
            else if (strncmp(buffer, "UNBLOCK_DOMAIN", 14) == 0) {
                char *domain = buffer + 15;
                domain_blacklist_remove(domain);
                snprintf(response, sizeof(response), "Domain unblocked.\n");
            }
            else if (strncmp(buffer, "FORCE_DISCONNECT", 16) == 0) {
                int session_id = atoi(buffer + 17);
                if (force_disconnect_client(session_id)) {
                    snprintf(response, sizeof(response), "Client disconnected.\n");
                } else {
                    snprintf(response, sizeof(response), "Client not found.\n");
                }
            }
            else if (strcmp(buffer, "PING") == 0) {
                snprintf(response, sizeof(response), "PONG");
            }
            else if (strcmp(buffer, "EXIT") == 0) {
                snprintf(response, sizeof(response), "Goodbye\n");
            }
            else {
                snprintf(response, sizeof(response), "Comenzi: STATS, CLIENTS, HISTORY, QUEUE, AVG_TIME, SESSIONS, PROCESSES, KILL <id>, EXIT\n");
            }
            
            // Trimite raspunsul folosind sendto
            ssize_t sent = sendto(sock, response, strlen(response), 0,
                   (struct sockaddr *)&client_addr, client_addr_len);
char debug[70000];
snprintf(debug, sizeof(debug), "[UNIX] Sent response: %ld bytes, response='%s'", (long)sent, response);
log_message(debug);
        }
    }
    
    close(sock);
    unlink(socket_path);
    pthread_exit(NULL);
}
    