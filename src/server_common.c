#include "../include/logging.h"
#include "../include/proto.h"
#include <fcntl.h>
#include <stdlib.h>

static server_stats_t g_stats = {0, 0, 0, 0.0, ""};
static pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;

static client_session_t *sessions = NULL;
static int next_session_id = 1000;
static pthread_mutex_t session_mutex = PTHREAD_MUTEX_INITIALIZER;

#define MAX_HISTORY 100
typedef struct {
    char command[512];
    time_t timestamp;
    long execution_time_ms;
} history_entry_t;

static history_entry_t history[MAX_HISTORY];
static int history_count = 0;
static pthread_mutex_t history_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct queue_task {
    int task_id;
    char filename[512];
    char bbox[128];
    double epsilon;
    int show_segments;
    int dist_idx1;
    int dist_idx2;
    int client_id;
    int sock_fd;
    int status;
    time_t start_time;
    time_t end_time;
    pointMsgType *points;
    int point_count;
    struct queue_task *next;
} queue_task_t;

static queue_task_t *queue_head = NULL;
static queue_task_t *queue_tail = NULL;
static int next_task_id = 1;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

void add_to_history(const char *command, long exec_time_ms);

static const char* get_password_file(void) {
    const char *env_file = getenv("GEO_PASSWD_FILE");
    if (env_file != NULL && env_file[0] != '\0') {
        return env_file;
    }
    return "passwords.txt";
}

void add_to_history(const char *command, long exec_time_ms) {
    pthread_mutex_lock(&history_mutex);
    if (history_count < MAX_HISTORY) {
        strncpy(history[history_count].command, command, sizeof(history[0].command) - 1);
        history[history_count].command[sizeof(history[0].command) - 1] = '\0';
        history[history_count].timestamp = time(NULL);
        history[history_count].execution_time_ms = exec_time_ms;
        history_count++;
    } else {
        for (int i = 1; i < MAX_HISTORY; i++) {
            memcpy(&history[i-1], &history[i], sizeof(history_entry_t));
        }
        strncpy(history[MAX_HISTORY-1].command, command, sizeof(history[0].command) - 1);
        history[MAX_HISTORY-1].command[sizeof(history[0].command) - 1] = '\0';
        history[MAX_HISTORY-1].timestamp = time(NULL);
        history[MAX_HISTORY-1].execution_time_ms = exec_time_ms;
    }
    pthread_mutex_unlock(&history_mutex);
}

void format_history_response(char *buffer, size_t bufsize) {
    pthread_mutex_lock(&history_mutex);
    buffer[0] = '\0';
    
    int start = 0;
    int end = history_count;
    if (history_count > 10) {
        start = history_count - 10;
    }
    
    for (int i = start; i < end; i++) {
        char timebuf[64];
        struct tm *tm_info = localtime(&history[i].timestamp);
        strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm_info);
        
        char line[600];
        snprintf(line, sizeof(line), "%d. [%s] %s\n", i + 1, timebuf, history[i].command);
        strncat(buffer, line, bufsize - strlen(buffer) - 1);
    }
    
    if (history_count == 0) {
        snprintf(buffer, bufsize, "Niciun istoric disponibil\n");
    }
    pthread_mutex_unlock(&history_mutex);
}

void format_avg_time_response(char *buffer, size_t bufsize) {
    pthread_mutex_lock(&history_mutex);
    if (history_count == 0) {
        snprintf(buffer, bufsize, "Nicio comanda procesata inca\n");
    } else {
        long total_time_ms = 0;
        for (int i = 0; i < history_count; i++) {
            total_time_ms += history[i].execution_time_ms;
        }
        double avg_time_sec = (double)total_time_ms / (double)history_count / 1000.0;
        snprintf(buffer, bufsize, "Total comenzi: %d\nTimp mediu: %.2f secunde\n", 
                 history_count, avg_time_sec);
    }
    pthread_mutex_unlock(&history_mutex);
}

static void process_task_real(queue_task_t *task) {
    pointMsgType *points = task->points;
    int point_count = task->point_count;
    
    if (task->bbox[0] != '\0') {
        double min_lat, max_lat, min_lon, max_lon;
        if (sscanf(task->bbox, "%lf,%lf,%lf,%lf", &min_lat, &max_lat, &min_lon, &max_lon) == 4) {
            pointMsgType *filtered = malloc(sizeof(pointMsgType) * point_count);
            int filtered_count = 0;
            for (int i = 0; i < point_count; i++) {
                if (points[i].lat >= min_lat && points[i].lat <= max_lat &&
                    points[i].lon >= min_lon && points[i].lon <= max_lon) {
                    filtered[filtered_count++] = points[i];
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
    }
    
    if (task->epsilon > 0 && point_count > 2) {
        pointMsgType *simplified = NULL;
        int simplified_count = douglas_peucker(points, point_count, task->epsilon, &simplified);
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
    
    for (int i = 0; i < point_count - 1; i++) {
        double dist = haversine_distance(points[i], points[i+1]);
        segment_distances[i] = dist;
        total_distance += dist;
    }
    
    double direct_distance = 0.0;
    double route_distance = 0.0;
    int has_distance_request = (task->dist_idx1 > 0 && task->dist_idx2 > 0 && 
                                 task->dist_idx1 <= point_count && task->dist_idx2 <= point_count);
    
    if (has_distance_request) {
        int idx1 = task->dist_idx1 - 1;
        int idx2 = task->dist_idx2 - 1;
        direct_distance = haversine_distance(points[idx1], points[idx2]);
        
        int start = (idx1 < idx2) ? idx1 : idx2;
        int end = (idx1 > idx2) ? idx1 : idx2;
        route_distance = 0.0;
        for (int i = start; i < end; i++) {
            route_distance += haversine_distance(points[i], points[i+1]);
        }
    }
    
    msgHeaderType h;
    h.clientID = task->client_id;
    h.opID = OPR_UPLOAD_GEO;
    
    char total_dist_str[64], point_cnt_str[64], seg_cnt_str[64];
    char direct_dist_str[64], route_dist_str[64], has_req_str[64];
    char show_seg_str[16];
    
    snprintf(total_dist_str, sizeof(total_dist_str), "%.6f", total_distance);
    snprintf(point_cnt_str, sizeof(point_cnt_str), "%d", point_count);
    snprintf(seg_cnt_str, sizeof(seg_cnt_str), "%d", segment_count);
    snprintf(direct_dist_str, sizeof(direct_dist_str), "%.6f", direct_distance);
    snprintf(route_dist_str, sizeof(route_dist_str), "%.6f", route_distance);
    snprintf(has_req_str, sizeof(has_req_str), "%d", has_distance_request);
    snprintf(show_seg_str, sizeof(show_seg_str), "%d", task->show_segments);
    
    writeSingleString(task->sock_fd, h, total_dist_str);
    writeSingleString(task->sock_fd, h, point_cnt_str);
    writeSingleString(task->sock_fd, h, seg_cnt_str);
    
    for (int i = 0; i < segment_count; i++) {
        char seg_str[64];
        snprintf(seg_str, sizeof(seg_str), "%.6f", segment_distances[i]);
        writeSingleString(task->sock_fd, h, seg_str);
    }
    
    writeSingleString(task->sock_fd, h, direct_dist_str);
    writeSingleString(task->sock_fd, h, route_dist_str);
    writeSingleString(task->sock_fd, h, has_req_str);
    writeSingleString(task->sock_fd, h, show_seg_str);
    
    stats_add_processed(point_count, total_distance, task->filename);
    
    char logbuf[256];
    snprintf(logbuf, sizeof(logbuf), "[GEO] Task %d: %d points, distance=%.2f km", 
             task->task_id, point_count, total_distance);
    log_message(logbuf);
    
    free(points);
}

int queue_add_task_full(const char *filename, int client_id, int sock_fd, int point_count,
                        const char *bbox, double epsilon, int show_segments,
                        int dist_idx1, int dist_idx2, pointMsgType *points) {
    queue_task_t *task = malloc(sizeof(queue_task_t));
    if (!task) return -1;
    
    task->task_id = next_task_id++;
    strncpy(task->filename, filename, sizeof(task->filename) - 1);
    task->filename[sizeof(task->filename) - 1] = '\0';
    
    if (bbox) {
        strncpy(task->bbox, bbox, sizeof(task->bbox) - 1);
        task->bbox[sizeof(task->bbox) - 1] = '\0';
    } else {
        task->bbox[0] = '\0';
    }
    
    task->epsilon = epsilon;
    task->show_segments = show_segments;
    task->dist_idx1 = dist_idx1;
    task->dist_idx2 = dist_idx2;
    task->client_id = client_id;
    task->sock_fd = sock_fd;
    task->status = 0;
    task->start_time = time(NULL);
    task->end_time = 0;
    task->points = points;
    task->point_count = point_count;
    task->next = NULL;
    
    pthread_mutex_lock(&queue_mutex);
    if (queue_tail) {
        queue_tail->next = task;
        queue_tail = task;
    } else {
        queue_head = queue_tail = task;
    }
    pthread_cond_signal(&queue_cond);
    pthread_mutex_unlock(&queue_mutex);
    
    char logbuf[256];
    snprintf(logbuf, sizeof(logbuf), "[QUEUE] Task %d added: %s (%d points)", 
             task->task_id, filename, point_count);
    log_message(logbuf);
    
    return task->task_id;
}

int queue_add_task(const char *filename, int client_id) {
    return queue_add_task_full(filename, client_id, -1, 0, NULL, -1, 0, 0, 0, NULL);
}

void format_queue_response(char *buffer, size_t bufsize) {
    pthread_mutex_lock(&queue_mutex);
    buffer[0] = '\0';
    queue_task_t *curr = queue_head;
    int count = 0;
    
    if (curr == NULL) {
        snprintf(buffer, bufsize, "Coada este goala\n");
    } else {
        while (curr && count < 10) {
            char status_str[32];
            if (curr->status == 0) {
                snprintf(status_str, sizeof(status_str), "In asteptare");
            } else if (curr->status == 1) {
                snprintf(status_str, sizeof(status_str), "In procesare");
            } else {
                snprintf(status_str, sizeof(status_str), "Finalizat");
            }
            
            char line[600];
            snprintf(line, sizeof(line), "Task %d: %s [%s] (%d puncte)\n", 
                     curr->task_id, curr->filename, status_str, curr->point_count);
            strncat(buffer, line, bufsize - strlen(buffer) - 1);
            curr = curr->next;
            count++;
        }
    }
    pthread_mutex_unlock(&queue_mutex);
}

void *queue_processor(void *arg) {
    (void)arg;
    char logbuf[128];
    char hist_entry[600];
    
    while (1) {
        pthread_mutex_lock(&queue_mutex);
        
        while (queue_head == NULL) {
            pthread_cond_wait(&queue_cond, &queue_mutex);
        }
        
        queue_task_t *task = queue_head;
        queue_head = queue_head->next;
        if (queue_head == NULL) queue_tail = NULL;
        
        pthread_mutex_unlock(&queue_mutex);
        
        task->status = 1;
        snprintf(logbuf, sizeof(logbuf), "[QUEUE] Processing task %d (%d points)", 
                 task->task_id, task->point_count);
        log_message(logbuf);
        
        process_task_real(task);
        
        task->status = 2;
        task->end_time = time(NULL);
        
        long exec_time_ms = (task->end_time - task->start_time) * 1000;
        snprintf(logbuf, sizeof(logbuf), "[QUEUE] Task %d completed in %ld ms", 
                 task->task_id, exec_time_ms);
        log_message(logbuf);
        
        snprintf(hist_entry, sizeof(hist_entry), "Task %d: %s (%d puncte)", 
                 task->task_id, task->filename, task->point_count);
        add_to_history(hist_entry, exec_time_ms);
        
        free(task);
    }
    return NULL;
}

int session_create(const char *username) {
    pthread_mutex_lock(&session_mutex);
    
    client_session_t *new_session = malloc(sizeof(client_session_t));
    if (!new_session) {
        pthread_mutex_unlock(&session_mutex);
        return -1;
    }
    
    new_session->session_id = next_session_id++;
    strncpy(new_session->username, username, sizeof(new_session->username) - 1);
    new_session->authenticated = 1;
    new_session->login_time = time(NULL);
    new_session->last_activity = time(NULL);
    new_session->next = sessions;
    sessions = new_session;
    
    int session_id = new_session->session_id;
    pthread_mutex_unlock(&session_mutex);
    
    char logbuf[256];
    snprintf(logbuf, sizeof(logbuf), "[SESSION] Created session %d for user %s", session_id, username);
    log_message(logbuf);
    
    return session_id;
}

int session_validate(int session_id) {
    pthread_mutex_lock(&session_mutex);
    
    client_session_t *curr = sessions;
    while (curr) {
        if (curr->session_id == session_id && curr->authenticated) {
            pthread_mutex_unlock(&session_mutex);
            return 1;
        }
        curr = curr->next;
    }
    
    pthread_mutex_unlock(&session_mutex);
    return 0;
}

void session_invalidate(int session_id) {
    pthread_mutex_lock(&session_mutex);
    
    client_session_t *curr = sessions;
    client_session_t *prev = NULL;
    
    while (curr) {
        if (curr->session_id == session_id) {
            if (prev) {
                prev->next = curr->next;
            } else {
                sessions = curr->next;
            }
            char logbuf[256];
            snprintf(logbuf, sizeof(logbuf), "[SESSION] Invalidated session %d", session_id);
            log_message(logbuf);
            free(curr);
            break;
        }
        prev = curr;
        curr = curr->next;
    }
    
    pthread_mutex_unlock(&session_mutex);
}

void session_update_activity(int session_id) {
    pthread_mutex_lock(&session_mutex);
    
    client_session_t *curr = sessions;
    while (curr) {
        if (curr->session_id == session_id) {
            curr->last_activity = time(NULL);
            break;
        }
        curr = curr->next;
    }
    
    pthread_mutex_unlock(&session_mutex);
}

int authenticate_user(const char *user, const char *pass) {
    const char *passwd_file = get_password_file();
    int fd = open(passwd_file, O_RDONLY);
    if (fd < 0) {
        char logbuf[256];
        snprintf(logbuf, sizeof(logbuf), "[AUTH] Cannot open passwords file: %s", passwd_file);
        log_message(logbuf);
        return 0;
    }
    
    char line[256];
    int pos = 0;
    char c;
    int found = 0;
    
    while (read(fd, &c, 1) > 0) {
        if (c == '\n') {
            line[pos] = '\0';
            char *sep = strchr(line, ' ');
            if (sep) {
                *sep = '\0';
                if (strcmp(line, user) == 0 && strcmp(sep + 1, pass) == 0) {
                    found = 1;
                    break;
                }
            }
            pos = 0;
        } else if (pos < (int)sizeof(line) - 1) {
            line[pos++] = c;
        }
    }
    close(fd);
    
    char logbuf[256];
    snprintf(logbuf, sizeof(logbuf), "[AUTH] User %s auth result: %d (using %s)", user, found, passwd_file);
    log_message(logbuf);
    
    return found;
}

int add_user_server(const char *user, const char *pass) {
    const char *passwd_file = get_password_file();
    int fd = open(passwd_file, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) {
        char logbuf[256];
        snprintf(logbuf, sizeof(logbuf), "[AUTH] Cannot open passwords file for writing: %s", passwd_file);
        log_message(logbuf);
        return 0;
    }
    
    char line[256];
    int len = snprintf(line, sizeof(line), "%s %s\n", user, pass);
    write(fd, line, len);
    close(fd);
    
    char logbuf[256];
    snprintf(logbuf, sizeof(logbuf), "[AUTH] New user registered: %s (in %s)", user, passwd_file);
    log_message(logbuf);
    
    return 1;
}

void stats_increment_clients(void) {
    pthread_mutex_lock(&stats_mutex);
    g_stats.active_clients++;
    pthread_mutex_unlock(&stats_mutex);
}

void stats_decrement_clients(void) {
    pthread_mutex_lock(&stats_mutex);
    if (g_stats.active_clients > 0) g_stats.active_clients--;
    pthread_mutex_unlock(&stats_mutex);
}

void stats_add_processed(int points, double distance, const char *filename) {
    pthread_mutex_lock(&stats_mutex);
    g_stats.total_processed_points += points;
    g_stats.total_processed_distance += distance;
    strncpy(g_stats.last_upload, filename, sizeof(g_stats.last_upload) - 1);
    pthread_mutex_unlock(&stats_mutex);
}
void stats_increment_processes(void) {
    pthread_mutex_lock(&stats_mutex);
    g_stats.active_processes++;
    pthread_mutex_unlock(&stats_mutex);
}

void stats_decrement_processes(void) {
    pthread_mutex_lock(&stats_mutex);
    if (g_stats.active_processes > 0) g_stats.active_processes--;
    pthread_mutex_unlock(&stats_mutex);
}

void get_stats(server_stats_t *stats) {
    pthread_mutex_lock(&stats_mutex);
    memcpy(stats, &g_stats, sizeof(server_stats_t));
    pthread_mutex_unlock(&stats_mutex);
}

void format_sessions_response(char *buffer, size_t bufsize) {
    pthread_mutex_lock(&session_mutex);
    buffer[0] = '\0';
    client_session_t *curr = sessions;
    int count = 0;
    
    if (curr == NULL) {
        snprintf(buffer, bufsize, "Nicio sesiune activa\n");
    } else {
        while (curr && count < 20) {
            char timebuf[64];
            struct tm *tm_info = localtime(&curr->login_time);
            strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm_info);
            
            long duration = time(NULL) - curr->login_time;
            
            char line[600];
            snprintf(line, sizeof(line), "Session %d: %s (login: %s, activ: %ld sec)\n", 
                     curr->session_id, curr->username, timebuf, duration);
            strncat(buffer, line, bufsize - strlen(buffer) - 1);
            curr = curr->next;
            count++;
        }
    }
    pthread_mutex_unlock(&session_mutex);
}

int terminate_session(int session_id) {
    pthread_mutex_lock(&session_mutex);
    client_session_t *curr = sessions;
    client_session_t *prev = NULL;
    int found = 0;
    
    while (curr) {
        if (curr->session_id == session_id) {
            if (prev) {
                prev->next = curr->next;
            } else {
                sessions = curr->next;
            }
            char logbuf[256];
            snprintf(logbuf, sizeof(logbuf), "[ADMIN] Terminated session %d for user %s", 
                     session_id, curr->username);
            log_message(logbuf);
            free(curr);
            found = 1;
            break;
        }
        prev = curr;
        curr = curr->next;
    }
    pthread_mutex_unlock(&session_mutex);
    return found;
}