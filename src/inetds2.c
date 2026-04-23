#include "../include/logging.h"
#include "../include/proto.h"
#include "../include/config.h"
#include <fcntl.h>
#include <stdlib.h>

extern int session_create(const char *username);
extern int session_validate(int session_id);
extern void session_invalidate(int session_id);
extern void session_update_activity(int session_id);
extern int authenticate_user(const char *user, const char *pass);
extern int add_user_server(const char *user, const char *pass);
extern void stats_increment_clients(void);
extern void stats_decrement_clients(void);
extern void stats_add_processed(int points, double distance, const char *filename);
extern void stats_increment_processes(void);
extern void stats_decrement_processes(void);
extern void get_stats(server_stats_t *stats);
extern void add_to_history(const char *command);
extern int queue_add_task_full(const char *filename, int client_id, int sock_fd, 
                                int point_count, const char *bbox, double epsilon, 
                                int show_segments, int dist_idx1, int dist_idx2, 
                                pointMsgType *points);

int inet_socket(uint16_t port, short reuse) {
    int sock;
    struct sockaddr_in name;
    
    sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    
    if (reuse) {
        int reuseAddrON = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuseAddrON, sizeof(reuseAddrON));
    }
    
    name.sin_family = AF_INET;
    name.sin_port = htons(port);
    name.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&name, sizeof(name)) < 0) {
        close(sock);
        return -1;
    }
    
    return sock;
}

void *inet_main(void *args) {
    int port = *((int *)args);
    int sock;
    size_t size;
    fd_set active_fd_set, read_fd_set;
    struct sockaddr_in clientname;
    char logbuf[256];
    
    snprintf(logbuf, sizeof(logbuf), "[INET] Starting INET server on port %d...", port);
    write(STDOUT_FILENO, logbuf, strlen(logbuf));
    write(STDOUT_FILENO, "\n", 1);
    
    sock = inet_socket(port, 1);
    if (sock < 0) {
        write(STDOUT_FILENO, "[INET] Failed to create socket\n", 32);
        pthread_exit(NULL);
    }
    
    if (listen(sock, g_config.max_clients) < 0) {
        write(STDOUT_FILENO, "[INET] Failed to listen\n", 24);
        pthread_exit(NULL);
    }
    
    FD_ZERO(&active_fd_set);
    FD_SET(sock, &active_fd_set);
    
    while (1) {
        int i;
        read_fd_set = active_fd_set;
        
        if (select(FD_SETSIZE, &read_fd_set, NULL, NULL, NULL) < 0) {
            continue;
        }
        
        for (i = 0; i < FD_SETSIZE; ++i) {
            if (FD_ISSET(i, &read_fd_set)) {
                if (i == sock) {
                    int new_fd;
                    size = sizeof(clientname);
                    new_fd = accept(sock, (struct sockaddr *)&clientname, (socklen_t *)&size);
                    if (new_fd >= 0) {
                        FD_SET(new_fd, &active_fd_set);
                        stats_increment_clients();
                        snprintf(logbuf, sizeof(logbuf), "[INET] New connection on fd=%d", new_fd);
                        write(STDOUT_FILENO, logbuf, strlen(logbuf));
                        write(STDOUT_FILENO, "\n", 1);
                    }
                } else {
                    msgHeaderType h = peekMsgHeader(i);
                    int operation = h.opID;
                    int client_id = h.clientID;
                    
                    if (operation == 0 && client_id == 0) {
                        msgIntType dummy;
                        readSingleInt(i, &dummy);
                        int new_id = (int)time(NULL);
                        writeSingleInt(i, h, new_id);
                        snprintf(logbuf, sizeof(logbuf), "[INET] New client assigned ID=%d", new_id);
                        write(STDOUT_FILENO, logbuf, strlen(logbuf));
                        write(STDOUT_FILENO, "\n", 1);
                    }
                    else if (operation == OPR_LOGIN) {
                        msgStringType user, pass;
                        if (readSingleString(i, &user) < 0 || readSingleString(i, &pass) < 0) {
                            char err_msg[] = "Eroare la autentificare";
                            writeSingleString(i, h, err_msg);
                            continue;
                        }
                        
                        int auth_result = authenticate_user(user.msg, pass.msg);
                        if (auth_result) {
                            int session_id = session_create(user.msg);
                            writeSingleInt(i, h, session_id);
                            snprintf(logbuf, sizeof(logbuf), "[INET] User %s authenticated, session=%d", user.msg, session_id);
                            write(STDOUT_FILENO, logbuf, strlen(logbuf));
                            write(STDOUT_FILENO, "\n", 1);
                            
                            char hist_entry[256];
                            snprintf(hist_entry, sizeof(hist_entry), "Login: %s", user.msg);
                            add_to_history(hist_entry);
                        } else {
                            writeSingleInt(i, h, 0);
                            snprintf(logbuf, sizeof(logbuf), "[INET] Auth failed for user %s", user.msg);
                            write(STDOUT_FILENO, logbuf, strlen(logbuf));
                            write(STDOUT_FILENO, "\n", 1);
                        }
                        free(user.msg);
                        free(pass.msg);
                    }
                    else if (operation == OPR_REGISTER) {
                        msgStringType user, pass;
                        if (readSingleString(i, &user) < 0 || readSingleString(i, &pass) < 0) {
                            char err_msg[] = "Eroare la inregistrare";
                            writeSingleString(i, h, err_msg);
                            continue;
                        }
                        
                        int reg_result = add_user_server(user.msg, pass.msg);
                        if (reg_result) {
                            int session_id = session_create(user.msg);
                            writeSingleInt(i, h, session_id);
                            snprintf(logbuf, sizeof(logbuf), "[INET] New user registered: %s, session=%d", user.msg, session_id);
                            write(STDOUT_FILENO, logbuf, strlen(logbuf));
                            write(STDOUT_FILENO, "\n", 1);
                            
                            char hist_entry[256];
                            snprintf(hist_entry, sizeof(hist_entry), "Register: %s", user.msg);
                            add_to_history(hist_entry);
                        } else {
                            writeSingleInt(i, h, 0);
                            write(STDOUT_FILENO, "[INET] Registration failed\n", 28);
                        }
                        free(user.msg);
                        free(pass.msg);
                    }
                    else if (operation == OPR_UPLOAD_GEO) {
                        int session_id = client_id;
                        if (!session_validate(session_id)) {
                            char err_msg[] = "Sesiune invalida";
                            writeSingleString(i, h, err_msg);
                            continue;
                        }
                        
                        session_update_activity(session_id);
                        
                        msgStringType filename;
                        if (readSingleString(i, &filename) < 0) {
                            char err_msg[] = "Eroare la citirea numelui fisierului";
                            writeSingleString(i, h, err_msg);
                            continue;
                        }
                        
                        msgStringType point_count_str;
                        if (readSingleString(i, &point_count_str) < 0) {
                            free(filename.msg);
                            char err_msg[] = "Eroare la citirea numarului de puncte";
                            writeSingleString(i, h, err_msg);
                            continue;
                        }
                        int point_count = atoi(point_count_str.msg);
                        free(point_count_str.msg);
                        
                        if (point_count <= 0 || point_count > 100000) {
                            free(filename.msg);
                            char err_msg[] = "Numar invalid de puncte";
                            writeSingleString(i, h, err_msg);
                            continue;
                        }
                        
                        msgStringType bbox_str;
                        char bbox[128] = "";
                        if (readSingleString(i, &bbox_str) >= 0 && strlen(bbox_str.msg) > 0) {
                            strncpy(bbox, bbox_str.msg, sizeof(bbox) - 1);
                            free(bbox_str.msg);
                        }
                        
                        msgStringType epsilon_str;
                        double epsilon = -1;
                        if (readSingleString(i, &epsilon_str) >= 0 && strlen(epsilon_str.msg) > 0) {
                            epsilon = atof(epsilon_str.msg);
                            free(epsilon_str.msg);
                        }
                        
                        msgStringType segments_flag_str;
                        int show_segments = 0;
                        if (readSingleString(i, &segments_flag_str) >= 0 && strlen(segments_flag_str.msg) > 0) {
                            show_segments = atoi(segments_flag_str.msg);
                            free(segments_flag_str.msg);
                        }
                        
                        msgStringType dist1_str, dist2_str;
                        int dist_idx1 = 0, dist_idx2 = 0;
                        if (readSingleString(i, &dist1_str) >= 0 && strlen(dist1_str.msg) > 0) {
                            dist_idx1 = atoi(dist1_str.msg);
                            free(dist1_str.msg);
                        }
                        if (readSingleString(i, &dist2_str) >= 0 && strlen(dist2_str.msg) > 0) {
                            dist_idx2 = atoi(dist2_str.msg);
                            free(dist2_str.msg);
                        }
                        
                        pointMsgType *points = malloc(sizeof(pointMsgType) * point_count);
                        if (!points) {
                            free(filename.msg);
                            char err_msg[] = "Eroare de memorie";
                            writeSingleString(i, h, err_msg);
                            continue;
                        }
                        
                        int points_read = 0;
                        for (int p = 0; p < point_count; p++) {
                            msgStringType coord_str;
                            if (readSingleString(i, &coord_str) < 0) break;
                            sscanf(coord_str.msg, "%lf,%lf", &points[p].lat, &points[p].lon);
                            free(coord_str.msg);
                            points_read++;
                        }
                        
                        if (points_read != point_count) {
                            free(points);
                            free(filename.msg);
                            char err_msg[] = "Eroare la citirea punctelor";
                            writeSingleString(i, h, err_msg);
                            continue;
                        }
                        
                        queue_add_task_full(filename.msg, session_id, i, point_count,
                                            bbox, epsilon, show_segments,
                                            dist_idx1, dist_idx2, points);
                        
                        free(filename.msg);
                        
                        char hist_entry[256];
                        snprintf(hist_entry, sizeof(hist_entry), "Upload: %s (%d puncte)", 
                                 filename.msg, point_count);
                        add_to_history(hist_entry);
                    }
                    else if (operation == OPR_BYE) {
                        int session_id = client_id;
                        if (session_id > 0) {
                            session_invalidate(session_id);
                        }
                        char bye_msg[] = "Deconectare realizata";
                        writeSingleString(i, h, bye_msg);
                        close(i);
                        FD_CLR(i, &active_fd_set);
                        stats_decrement_clients();
                    }
                    else {
                        char err_msg[] = "Comanda necunoscuta";
                        writeSingleString(i, h, err_msg);
                    }
                }
            }
        }
    }
    
    pthread_exit(NULL);
}