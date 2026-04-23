#include "../include/proto.h"      // Pentru structuri, constante si functii comune ale serverului
#include "../include/logging.h"    // Pentru logarea evenimentelor si erorilor
#include <../include/config.h>     // Pentru configuratia globala a serverului
#include <sys/time.h>              // Pentru struct timeval folosit la select() si timeout-uri

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
    char *socket_path = (char *)args;
    int sock, client_fd;
    struct sockaddr_un addr;
    socklen_t addr_len = sizeof(addr);
    fd_set read_fds;
    char buffer[4096];
    ssize_t bytes;
    int admin_connected = 0;
    struct timeval tv;
    
    // Creeaza socket-ul local de tip UNIX stream
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        log_message("[UNIX] socket creation failed");
        pthread_exit(NULL);
    }
    
    // Initializeaza adresa socket-ului UNIX
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';
    
    // Elimina vechiul socket, daca exista deja
    unlink(socket_path);
    
    // Leaga socket-ul la calea locala specificata
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        char errbuf[256];
        snprintf(errbuf, sizeof(errbuf), "[UNIX] bind failed: %s", strerror(errno));
        log_message(errbuf);
        pthread_exit(NULL);
    }
    
    // Trecerea socket-ului in stare de ascultare
    if (listen(sock, g_config.max_clients) < 0) {
        log_message("[UNIX] listen failed");
        pthread_exit(NULL);
    }
    
    char logbuf[256];
    snprintf(logbuf, sizeof(logbuf), "[UNIX] Trying to bind to path: '%s'", addr.sun_path);
    log_message(logbuf);
    
    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(sock, &read_fds);
        
        // Timeout scurt pentru a nu bloca indefinit bucla de monitorizare
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        if (select(sock + 1, &read_fds, NULL, NULL, &tv) < 0) {
            continue;
        }
        
        if (FD_ISSET(sock, &read_fds)) {
            // Daca exista deja un admin conectat, orice alta conexiune este refuzata
            if (admin_connected) {
                client_fd = accept(sock, (struct sockaddr *)&addr, &addr_len);
                if (client_fd >= 0) {
                    char msg[] = "Eroare: Un admin este deja conectat!\n";
                    write(client_fd, msg, sizeof(msg)-1);
                    close(client_fd);
                }
                continue;
            }
            
            // Accepta noua conexiune de administrare
            client_fd = accept(sock, (struct sockaddr *)&addr, &addr_len);
            if (client_fd < 0) continue;
            
            admin_connected = 1;
            log_message("[UNIX] Admin connected");
            
            // Seteaza timeout pentru receptia de la clientul admin
            struct timeval timeout;
            timeout.tv_sec = 60;
            timeout.tv_usec = 0;
            setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            
            while (admin_connected) {
                // Citeste o comanda text de la admin
                bytes = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
                
                if (bytes <= 0) {
                    break;
                }
                
                buffer[bytes] = '\0';
                
                // Elimina newline-ul de la final, daca exista
                if (buffer[bytes - 1] == '\n') buffer[bytes - 1] = '\0';
                
                // Comanda pentru statistici generale
                if (strcmp(buffer, "STATS") == 0) {
                    server_stats_t stats;
                    get_stats(&stats);
                    format_stats_response(&stats, buffer, sizeof(buffer));
                    write(client_fd, buffer, strlen(buffer));
                }
                // Comanda pentru afisarea clientilor activi si a configuratiei de baza
                else if (strcmp(buffer, "CLIENTS") == 0) {
                    format_clients_response(buffer, sizeof(buffer));
                    write(client_fd, buffer, strlen(buffer));
                }
                // Comanda pentru istoric
                else if (strcmp(buffer, "HISTORY") == 0) {
                    format_history_response(buffer, sizeof(buffer));
                    write(client_fd, buffer, strlen(buffer));
                }
                // Comanda pentru afisarea task-urilor din coada
                else if (strcmp(buffer, "QUEUE") == 0) {
                    format_queue_response(buffer, sizeof(buffer));
                    write(client_fd, buffer, strlen(buffer));
                }
                // Comanda pentru timpul mediu de executie
                else if (strcmp(buffer, "AVG_TIME") == 0) {
                    format_avg_time_response(buffer, sizeof(buffer));
                    write(client_fd, buffer, strlen(buffer));
                }
                // Comanda pentru afisarea sesiunilor active
                else if (strcmp(buffer, "SESSIONS") == 0) {
                    format_sessions_response(buffer, sizeof(buffer));
                    write(client_fd, buffer, strlen(buffer));
                }
                // Comanda de test pentru creare proces copil
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
                // Comanda pentru terminarea unei sesiuni dupa ID
                else if (strncmp(buffer, "KILL", 4) == 0) {
                    int session_id = atoi(buffer + 5);
                    if (terminate_session(session_id)) {
                        char msg[] = "Sesiune terminata\n";
                        write(client_fd, msg, sizeof(msg)-1);
                    } else {
                        char msg[] = "Sesiune negasita\n";
                        write(client_fd, msg, sizeof(msg)-1);
                    }
                }
                // Comanda simpla de verificare a conectivitatii
                else if (strcmp(buffer, "PING") == 0) {
                    write(client_fd, "PONG", 4);
                }
                // Inchide sesiunea de administrare
                else if (strcmp(buffer, "EXIT") == 0) {
                    break;
                }
                // Afiseaza lista de comenzi disponibile daca cererea nu este recunoscuta
                else {
                    char msg[] = "Comenzi: STATS, CLIENTS, HISTORY, QUEUE, AVG_TIME, SESSIONS, PROCESSES, KILL <id>, EXIT\n";
                    write(client_fd, msg, sizeof(msg)-1);
                }
            }
            
            // Inchide conexiunea admin dupa terminarea sesiunii
            close(client_fd);
            admin_connected = 0;
            log_message("[UNIX] Admin disconnected");
        }
    }
    
    close(sock);
    unlink(socket_path);
    pthread_exit(NULL);
}