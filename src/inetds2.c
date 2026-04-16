
#include "../include/logging.h"
#include "../include/proto.h"
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
extern int queue_add_task(const char *filename, int client_id);

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
    log_message(logbuf);
    
    sock = inet_socket(port, 1);
    if (sock < 0) {
        log_message("[INET] Failed to create socket");
        pthread_exit(NULL);
    }
    
    if (listen(sock, 10) < 0) {
        log_message("[INET] Failed to listen");
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
                        log_message(logbuf);
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
                        log_message(logbuf);
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
                            log_message(logbuf);
                            
                            char hist_entry[256];
                            snprintf(hist_entry, sizeof(hist_entry), "Login: %s", user.msg);
                            add_to_history(hist_entry);
                        } else {
                            writeSingleInt(i, h, 0);
                            snprintf(logbuf, sizeof(logbuf), "[INET] Auth failed for user %s", user.msg);
                            log_message(logbuf);
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
                            log_message(logbuf);
                            
                            char hist_entry[256];
                            snprintf(hist_entry, sizeof(hist_entry), "Register: %s", user.msg);
                            add_to_history(hist_entry);
                        } else {
                            writeSingleInt(i, h, 0);
                            log_message("[INET] Registration failed");
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
                        
                        queue_add_task(filename.msg, session_id);
                        
                        msgStringType bbox_str;
                        int has_bbox = 0;
                        double min_lat = 0, max_lat = 0, min_lon = 0, max_lon = 0;
                        if (readSingleString(i, &bbox_str) >= 0 && strlen(bbox_str.msg) > 0) {
                            if (sscanf(bbox_str.msg, "%lf,%lf,%lf,%lf", &min_lat, &max_lat, &min_lon, &max_lon) == 4) {
                                has_bbox = 1;
                            }
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
                        
                        if (has_bbox) {
                            pointMsgType *filtered = malloc(sizeof(pointMsgType) * point_count);
                            int filtered_count = 0;
                            for (int p = 0; p < point_count; p++) {
                                if (points[p].lat >= min_lat && points[p].lat <= max_lat &&
                                    points[p].lon >= min_lon && points[p].lon <= max_lon) {
                                    filtered[filtered_count++] = points[p];
                                }
                            }
                            if (filtered_count > 0) {
                                free(points);
                                points = filtered;
                                point_count = filtered_count;
                            } else {
                                free(filtered);
                            }
                        }
                        
                        if (epsilon > 0 && point_count > 2) {
                            pointMsgType *simplified = NULL;
                            int simplified_count = douglas_peucker(points, point_count, epsilon, &simplified);
                            if (simplified_count > 0 && simplified_count < point_count) {
                                free(points);
                                points = simplified;
                                point_count = simplified_count;
                            } else if (simplified_count > 0) {
                                free(simplified);
                            }
                        }
                        
                        double total_distance = 0.0;
                        double segment_distances[1000];
                        int segment_count = point_count - 1;
                        
                        for (int p = 0; p < point_count - 1; p++) {
                            double dist = haversine_distance(points[p], points[p+1]);
                            segment_distances[p] = dist;
                            total_distance += dist;
                        }
                        
                        double direct_distance = 0.0;
                        double route_distance = 0.0;
                        int has_distance_request = (dist_idx1 > 0 && dist_idx2 > 0 && 
                                                     dist_idx1 <= point_count && dist_idx2 <= point_count);
                        
                        if (has_distance_request) {
                            int idx1 = dist_idx1 - 1;
                            int idx2 = dist_idx2 - 1;
                            direct_distance = haversine_distance(points[idx1], points[idx2]);
                            
                            int start = (idx1 < idx2) ? idx1 : idx2;
                            int end = (idx1 > idx2) ? idx1 : idx2;
                            route_distance = 0.0;
                            for (int p = start; p < end; p++) {
                                route_distance += haversine_distance(points[p], points[p+1]);
                            }
                        }
                        
                        char total_dist_str[64], point_cnt_str[64], seg_cnt_str[64];
                        char direct_dist_str[64], route_dist_str[64], has_req_str[64];
                        char show_seg_str[16];
                        
                        snprintf(total_dist_str, sizeof(total_dist_str), "%.6f", total_distance);
                        snprintf(point_cnt_str, sizeof(point_cnt_str), "%d", point_count);
                        snprintf(seg_cnt_str, sizeof(seg_cnt_str), "%d", segment_count);
                        snprintf(direct_dist_str, sizeof(direct_dist_str), "%.6f", direct_distance);
                        snprintf(route_dist_str, sizeof(route_dist_str), "%.6f", route_distance);
                        snprintf(has_req_str, sizeof(has_req_str), "%d", has_distance_request);
                        snprintf(show_seg_str, sizeof(show_seg_str), "%d", show_segments);
                        
                        writeSingleString(i, h, total_dist_str);
                        writeSingleString(i, h, point_cnt_str);
                        writeSingleString(i, h, seg_cnt_str);
                        
                        for (int p = 0; p < segment_count; p++) {
                            char seg_str[64];
                            snprintf(seg_str, sizeof(seg_str), "%.6f", segment_distances[p]);
                            writeSingleString(i, h, seg_str);
                        }
                        
                        writeSingleString(i, h, direct_dist_str);
                        writeSingleString(i, h, route_dist_str);
                        writeSingleString(i, h, has_req_str);
                        writeSingleString(i, h, show_seg_str);
                        
                        stats_add_processed(point_count, total_distance, filename.msg);
                        
                        snprintf(logbuf, sizeof(logbuf), "[GEO] Session %d: %d points, distance=%.2f km", 
                                 session_id, point_count, total_distance);
                        log_message(logbuf);
                        
                        free(points);
                        free(filename.msg);
                    }
                    else if (operation == OPR_BYE) {
                        int session_id = client_id;
                        if (session_id > 0) {
                            session_invalidate(session_id);
                        }
                        char bye_msg[] = "Deconectare realizata";
                        writeSingleString(i, h, bye_msg);
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
