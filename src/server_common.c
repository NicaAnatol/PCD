
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
static char history[MAX_HISTORY][256];
static int history_count = 0;
static pthread_mutex_t history_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct queue_task {
    int task_id;
    char filename[256];
    int client_id;
    int status;
    time_t start_time;
    time_t end_time;
    struct queue_task *next;
} queue_task_t;

static queue_task_t *queue_head = NULL;
static queue_task_t *queue_tail = NULL;
static int next_task_id = 1;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

static const char* get_password_file(void) {
    const char *env_file = getenv("GEO_PASSWD_FILE");
    if (env_file != NULL && env_file[0] != '\0') {
        return env_file;
    }
    return "passwords.txt";
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
    
    char history_entry[256];
    snprintf(history_entry, sizeof(history_entry), "Upload: %s (%d puncte, %.2f km)", filename, points, distance);
    add_to_history(history_entry);
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

void add_to_history(const char *command) {
    pthread_mutex_lock(&history_mutex);
    if (history_count < MAX_HISTORY) {
        strncpy(history[history_count], command, sizeof(history[0]) - 1);
        history[history_count][sizeof(history[0]) - 1] = '\0';
        history_count++;
    } else {
        for (int i = 1; i < MAX_HISTORY; i++) {
            memcpy(history[i-1], history[i], sizeof(history[0]));
        }
        strncpy(history[MAX_HISTORY-1], command, sizeof(history[0]) - 1);
        history[MAX_HISTORY-1][sizeof(history[0]) - 1] = '\0';
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
        char line[300];
        snprintf(line, sizeof(line), "%d. %s\n", i + 1, history[i]);
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
        snprintf(buffer, bufsize, "Total comenzi: %d\nTimp mediu: ~2 secunde (simulat)\n", history_count);
    }
    pthread_mutex_unlock(&history_mutex);
}

int queue_add_task(const char *filename, int client_id) {
    queue_task_t *task = malloc(sizeof(queue_task_t));
    if (!task) return -1;
    
    task->task_id = next_task_id++;
    strncpy(task->filename, filename, sizeof(task->filename) - 1);
    task->filename[sizeof(task->filename) - 1] = '\0';
    task->client_id = client_id;
    task->status = 0;
    task->start_time = time(NULL);
    task->end_time = 0;
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
    snprintf(logbuf, sizeof(logbuf), "[QUEUE] Task %d added: %s", task->task_id, filename);
    log_message(logbuf);
    
    return task->task_id;
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
            char status_str[20];
            if (curr->status == 0) snprintf(status_str, sizeof(status_str), "In asteptare");
            else if (curr->status == 1) snprintf(status_str, sizeof(status_str), "In procesare");
            else snprintf(status_str, sizeof(status_str), "Finalizat");
            
            char line[300];
            snprintf(line, sizeof(line), "Task %d: %s (%s)\n", 
                     curr->task_id, curr->filename, status_str);
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
        snprintf(logbuf, sizeof(logbuf), "[QUEUE] Processing task %d", task->task_id);
        log_message(logbuf);
        
        sleep(2);
        
        task->status = 2;
        task->end_time = time(NULL);
        
        snprintf(logbuf, sizeof(logbuf), "[QUEUE] Task %d completed", task->task_id);
        log_message(logbuf);
        
        add_to_history(task->filename);
        free(task);
    }
    return NULL;
}
