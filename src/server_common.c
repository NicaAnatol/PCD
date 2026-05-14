#include "../include/logging.h"   // Pentru logarea evenimentelor si erorilor serverului
#include "../include/proto.h"     // Pentru structuri si tipuri comune folosite in server
#include <fcntl.h>                // Pentru operatii pe fisiere si flag-uri precum open()
#include <stdlib.h>               // Pentru functii utilitare, inclusiv getenv()

// Structura globala care retine statistici despre activitatea serverului.
// Accesul concurent este protejat cu mutex.
static server_stats_t g_stats = {0, 0, 0, 0.0, ""};
static int notify_pipe[2] = {-1, -1};  // pipe pentru notificare coadă
static pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;

// Lista de sesiuni active ale clientilor conectati.
// next_session_id este folosit pentru generarea unui ID unic pentru fiecare sesiune noua.
static client_session_t *sessions = NULL;
static int next_session_id = 1000;
static pthread_mutex_t session_mutex = PTHREAD_MUTEX_INITIALIZER;

// Numarul maxim de comenzi pastrate in istoric.
#define MAX_HISTORY 100
#define MAX_BLACKLIST 100
static char blacklist_ips[MAX_BLACKLIST][64];
static int blacklist_count = 0;
static pthread_mutex_t blacklist_mutex = PTHREAD_MUTEX_INITIALIZER;
static char blacklist_domains[MAX_BLACKLIST][128];
static int domain_blacklist_count = 0;
static pthread_mutex_t domain_blacklist_mutex = PTHREAD_MUTEX_INITIALIZER;
// Timpul de păstrare a task-urilor finalizate (în secunde)
#define COMPLETED_TASK_LIFETIME 300  // 5 minute

// O intrare din istoric retine comanda executata, momentul executiei si durata acesteia.
typedef struct {
    char command[512];          // descrierea comenzii sau a task-ului executat
    time_t timestamp;           // momentul la care a fost salvata in istoric
    long execution_time_ms;     // timpul de executie in milisecunde
} history_entry_t;

// Bufferul de istoric si datele asociate lui.
// history_count indica numarul actual de intrari valide.
static history_entry_t history[MAX_HISTORY];
static int history_count = 0;
static pthread_mutex_t history_mutex = PTHREAD_MUTEX_INITIALIZER;

// Structura folosita pentru un task din coada de procesare.
// Ea retine toate datele necesare pentru procesarea unei cereri GEO.


// Pointerii pentru inceputul si sfarsitul cozii de task-uri.
// queue_cond este folosit pentru a trezi thread-ul procesor cand apare un task nou.
static queue_task_t *queue_head = NULL;
static queue_task_t *queue_tail = NULL;
static int next_task_id = 1;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

queue_task_t *completed_head = NULL;
queue_task_t *completed_tail = NULL;
pthread_mutex_t completed_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int session_id;
    int sock_fd;
} session_sock_map_t;

#define MAX_SESSION_SOCK_MAP 100
static session_sock_map_t session_sock_map[MAX_SESSION_SOCK_MAP];
static int session_sock_map_count = 0;
static pthread_mutex_t session_sock_mutex = PTHREAD_MUTEX_INITIALIZER;


int init_notify_pipe(void) {
    if (pipe(notify_pipe) < 0) {
        log_message("[PIPE] Failed to create notify pipe");
        return -1;
    }
    log_message("[PIPE] Notify pipe created successfully");
    return 0;
}

int get_notify_pipe_read_fd(void) {
    return notify_pipe[0];
}

void notify_queue(void) {
    if (notify_pipe[1] != -1) {
        char c = 'x';
        write(notify_pipe[1], &c, 1);
    }
}


// Prototip pentru functia care adauga o intrare noua in istoric.
void add_to_history(const char *command, long exec_time_ms);

// Stabileste ce fisier de parole va fi folosit.
// Daca variabila de mediu GEO_PASSWD_FILE este setata, se foloseste acea cale.
// Altfel se foloseste fisierul implicit "passwords.txt".
static const char* get_password_file(void) {
    const char *env_file = getenv("GEO_PASSWD_FILE");
    if (env_file != NULL && env_file[0] != '\0') {
        return env_file;
    }
    return "passwords.txt";
}

// Adauga o comanda noua in istoric impreuna cu timpul ei de executie.
// Daca istoricul este plin, cea mai veche intrare este eliminata.
void add_to_history(const char *command, long exec_time_ms) {
    pthread_mutex_lock(&history_mutex);
    
    if (history_count < MAX_HISTORY) {
        // Cazul normal: mai exista loc liber in bufferul de istoric
        strncpy(history[history_count].command, command, sizeof(history[0].command) - 1);
        history[history_count].command[sizeof(history[0].command) - 1] = '\0';
        history[history_count].timestamp = time(NULL);
        history[history_count].execution_time_ms = exec_time_ms;
        history_count++;
    } else {
        // Daca s-a atins capacitatea maxima, toate intrarile se deplaseaza la stanga
        for (int i = 1; i < MAX_HISTORY; i++) {
            memcpy(&history[i-1], &history[i], sizeof(history_entry_t));
        }
        
        // Ultima pozitie este reutilizata pentru noua intrare
        strncpy(history[MAX_HISTORY-1].command, command, sizeof(history[0].command) - 1);
        history[MAX_HISTORY-1].command[sizeof(history[0].command) - 1] = '\0';
        history[MAX_HISTORY-1].timestamp = time(NULL);
        history[MAX_HISTORY-1].execution_time_ms = exec_time_ms;
    }
    
    pthread_mutex_unlock(&history_mutex);
}

// Formateaza istoricul intr-un buffer text pentru a putea fi trimis catre client sau afisat.
// Se afiseaza cel mult ultimele 10 intrari pentru a pastra raspunsul scurt.
void format_history_response(char *buffer, size_t bufsize) {
    pthread_mutex_lock(&history_mutex);
    buffer[0] = '\0';
    
    int start = 0;
    int end = history_count;
    
    // Daca istoricul este mai mare, se afiseaza doar ultimele 10 comenzi
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
    
    // Mesaj special daca nu exista nicio intrare in istoric
    if (history_count == 0) {
        snprintf(buffer, bufsize, "Niciun istoric disponibil\n");
    }
    
    pthread_mutex_unlock(&history_mutex);
}

// Formateaza un raspuns text cu statistici despre timpul mediu de executie al comenzilor.
// Rezultatul este scris in bufferul primit ca parametru.
void format_avg_time_response(char *buffer, size_t bufsize) {
    pthread_mutex_lock(&history_mutex);
    
    // Daca nu exista comenzi in istoric, se afiseaza un mesaj explicit
    if (history_count == 0) {
        snprintf(buffer, bufsize, "Nicio comanda procesata inca\n");
    } else {
        long total_time_ms = 0;
        
        // Se aduna timpii de executie ai tuturor comenzilor memorate
        for (int i = 0; i < history_count; i++) {
            total_time_ms += history[i].execution_time_ms;
        }
        
        // Conversie din milisecunde in secunde pentru afisare mai usor de inteles
        double avg_time_sec = (double)total_time_ms / (double)history_count / 1000.0;
        snprintf(buffer, bufsize, "Total comenzi: %d\nTimp mediu: %.2f secunde\n", 
                 history_count, avg_time_sec);
    }
    
    pthread_mutex_unlock(&history_mutex);
}


void blacklist_add(const char *ip) {
    pthread_mutex_lock(&blacklist_mutex);
    if (blacklist_count < MAX_BLACKLIST) {
        strncpy(blacklist_ips[blacklist_count], ip, 63);
        blacklist_ips[blacklist_count][63] = '\0';
        blacklist_count++;
        char logbuf[256];
        snprintf(logbuf, sizeof(logbuf), "[BLACKLIST] Added %s", ip);
        log_message(logbuf);
    }
    pthread_mutex_unlock(&blacklist_mutex);
}

void blacklist_remove(const char *ip) {
    pthread_mutex_lock(&blacklist_mutex);
    for (int i = 0; i < blacklist_count; i++) {
        if (strcmp(blacklist_ips[i], ip) == 0) {
            for (int j = i; j < blacklist_count - 1; j++) {
                strcpy(blacklist_ips[j], blacklist_ips[j+1]);
            }
            blacklist_count--;
            char logbuf[256];
            snprintf(logbuf, sizeof(logbuf), "[BLACKLIST] Removed %s", ip);
            log_message(logbuf);
            break;
        }
    }
    pthread_mutex_unlock(&blacklist_mutex);
}

int blacklist_check(const char *ip) {
    pthread_mutex_lock(&blacklist_mutex);
    for (int i = 0; i < blacklist_count; i++) {
        if (strcmp(blacklist_ips[i], ip) == 0) {
            pthread_mutex_unlock(&blacklist_mutex);
            return 1;
        }
    }
    pthread_mutex_unlock(&blacklist_mutex);
    return 0;
}

void domain_blacklist_add(const char *domain) {
    pthread_mutex_lock(&domain_blacklist_mutex);
    if (domain_blacklist_count < MAX_BLACKLIST) {
        strncpy(blacklist_domains[domain_blacklist_count], domain, 127);
        blacklist_domains[domain_blacklist_count][127] = '\0';
        domain_blacklist_count++;
        char logbuf[256];
        snprintf(logbuf, sizeof(logbuf), "[DOMAIN_BLACKLIST] Added %s", domain);
        log_message(logbuf);
    }
    pthread_mutex_unlock(&domain_blacklist_mutex);
}

void domain_blacklist_remove(const char *domain) {
    pthread_mutex_lock(&domain_blacklist_mutex);
    for (int i = 0; i < domain_blacklist_count; i++) {
        if (strcmp(blacklist_domains[i], domain) == 0) {
            for (int j = i; j < domain_blacklist_count - 1; j++) {
                strcpy(blacklist_domains[j], blacklist_domains[j+1]);
            }
            domain_blacklist_count--;
            char logbuf[256];
            snprintf(logbuf, sizeof(logbuf), "[DOMAIN_BLACKLIST] Removed %s", domain);
            log_message(logbuf);
            break;
        }
    }
    pthread_mutex_unlock(&domain_blacklist_mutex);
}

int domain_blacklist_check(const char *domain) {
    pthread_mutex_lock(&domain_blacklist_mutex);
    for (int i = 0; i < domain_blacklist_count; i++) {
        if (strcmp(blacklist_domains[i], domain) == 0) {
            pthread_mutex_unlock(&domain_blacklist_mutex);
            return 1;
        }
    }
    pthread_mutex_unlock(&domain_blacklist_mutex);
    return 0;
}


void session_sock_add(int session_id, int sock_fd) {
    pthread_mutex_lock(&session_sock_mutex);
    if (session_sock_map_count < MAX_SESSION_SOCK_MAP) {
        session_sock_map[session_sock_map_count].session_id = session_id;
        session_sock_map[session_sock_map_count].sock_fd = sock_fd;
        session_sock_map_count++;
    }
    pthread_mutex_unlock(&session_sock_mutex);
}

// Elimină maparea pentru un session_id
void session_sock_remove(int session_id) {
    pthread_mutex_lock(&session_sock_mutex);
    for (int i = 0; i < session_sock_map_count; i++) {
        if (session_sock_map[i].session_id == session_id) {
            for (int j = i; j < session_sock_map_count - 1; j++) {
                session_sock_map[j] = session_sock_map[j+1];
            }
            session_sock_map_count--;
            break;
        }
    }
    pthread_mutex_unlock(&session_sock_mutex);
}

// Obține sock_fd pentru un session_id
int session_sock_get_fd(int session_id) {
    pthread_mutex_lock(&session_sock_mutex);
    for (int i = 0; i < session_sock_map_count; i++) {
        if (session_sock_map[i].session_id == session_id) {
            int fd = session_sock_map[i].sock_fd;
            pthread_mutex_unlock(&session_sock_mutex);
            return fd;
        }
    }
    pthread_mutex_unlock(&session_sock_mutex);
    return -1;
}

// Force disconnect a client by session ID
int force_disconnect_client(int session_id) {
    pthread_mutex_lock(&session_mutex);
    client_session_t *curr = sessions;
    client_session_t *prev = NULL;
    int found = 0;
    
    while (curr) {
        if (curr->session_id == session_id) {
            // Scoate sesiunea din listă
            if (prev) {
                prev->next = curr->next;
            } else {
                sessions = curr->next;
            }
            
            // Logheaza evenimentul
            char logbuf[256];
            snprintf(logbuf, sizeof(logbuf), "[ADMIN] Force disconnected session %d for user %s", 
                     session_id, curr->username);
            log_message(logbuf);
            
            // Curăță memoria sesiunii
            free(curr);
            found = 1;
            break;
        }
        prev = curr;
        curr = curr->next;
    }
    pthread_mutex_unlock(&session_mutex);
    
    // Închide socket-ul clientului dacă există
    int client_fd = session_sock_get_fd(session_id);
    if (client_fd > 0) {
        shutdown(client_fd, SHUT_RDWR);
        close(client_fd);
        session_sock_remove(session_id);
        
        char logbuf[256];
        snprintf(logbuf, sizeof(logbuf), "[ADMIN] Closed socket %d for session %d", client_fd, session_id);
        log_message(logbuf);
    }
    
    return found;
}
// Thread pentru curățarea task-urilor finalizate vechi
void *completed_task_cleanup(void *arg) {
    (void)arg;
    while (1) {
        sleep(60); // Verifică la fiecare minut
        
        pthread_mutex_lock(&completed_mutex);
        time_t now = time(NULL);
        queue_task_t *curr = completed_head;
        queue_task_t *prev = NULL;
        
        while (curr) {
            // Dacă task-ul este mai vechi decât LIFETIME
            if (now - curr->end_time > COMPLETED_TASK_LIFETIME) {
                queue_task_t *to_free = curr;
                int task_id = to_free->task_id;  
                
                if (prev) {
                    prev->next = curr->next;
                } else {
                    completed_head = curr->next;
                }
                
                if (curr->next == NULL) {
                    completed_tail = prev;
                }
                
                curr = curr->next;
                
                // Eliberează memoria
                if (to_free->points) free(to_free->points);
                free(to_free);
                
                char logbuf[256];
                snprintf(logbuf, sizeof(logbuf), "[CLEANUP] Removed old completed task %d", task_id);
                log_message(logbuf);
            } else {
                prev = curr;
                curr = curr->next;
            }
        }
        pthread_mutex_unlock(&completed_mutex);
    }
    return NULL;
}



// Proceseaza efectiv un task GEO extras din coada.
static void process_task_real(queue_task_t *task) {
    pointMsgType *points = task->points;
    int point_count = task->point_count;
    double *segment_distances = NULL;

    char debug_buf[1024];
    snprintf(debug_buf, sizeof(debug_buf), "[DEBUG] process_task_real START: task_id=%d, points=%p, point_count=%d, upload_path=%s", 
             task->task_id, (void*)points, point_count, task->upload_path);
    log_message(debug_buf);

    // Dacă nu avem puncte în memorie, încarcă din fișierul uploadat
    if (points == NULL && task->upload_path[0] != '\0') {
        snprintf(debug_buf, sizeof(debug_buf), "[DEBUG] Loading points from file: %s", task->upload_path);
        log_message(debug_buf);
        
        // Determină tipul după extensie
        const char *ext = strrchr(task->filename, '.');
        if (ext && (strcmp(ext, ".gpx") == 0 || strcmp(ext, ".GPX") == 0)) {
            point_count = parse_gpx(task->upload_path, &points);
            snprintf(debug_buf, sizeof(debug_buf), "[DEBUG] parse_gpx returned %d points", point_count);
            log_message(debug_buf);
        } else if (ext && (strcmp(ext, ".geojson") == 0 || strcmp(ext, ".json") == 0)) {
            point_count = parse_geojson(task->upload_path, &points);
            snprintf(debug_buf, sizeof(debug_buf), "[DEBUG] parse_geojson returned %d points", point_count);
            log_message(debug_buf);
        } else {
            point_count = parse_csv(task->upload_path, &points);
            snprintf(debug_buf, sizeof(debug_buf), "[DEBUG] parse_csv returned %d points", point_count);
            log_message(debug_buf);
        }

        if (point_count <= 0) {
            log_message("[ERROR] Failed to parse uploaded file");
            task->status = 4;
            return;
        }
        task->points = points;
        task->point_count = point_count;
        
        snprintf(debug_buf, sizeof(debug_buf), "[DEBUG] After loading: points=%p, point_count=%d", (void*)points, point_count);
        log_message(debug_buf);
    }
    
    // Filtrare optionala a punctelor daca task-ul contine un bounding box valid
    if (task->bbox[0] != '\0') {
        snprintf(debug_buf, sizeof(debug_buf), "[DEBUG] Applying bbox filter: %s", task->bbox);
        log_message(debug_buf);
        
        double min_lat, max_lat, min_lon, max_lon;
        if (sscanf(task->bbox, "%lf,%lf,%lf,%lf", &min_lat, &max_lat, &min_lon, &max_lon) == 4) {
            pointMsgType *filtered = malloc(sizeof(pointMsgType) * point_count);
            if (!filtered) {
                log_message("[ERROR] Failed to allocate filtered points");
                task->status = 4;
                return;
            }
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
                snprintf(debug_buf, sizeof(debug_buf), "[DEBUG] After bbox filter: point_count=%d", point_count);
                log_message(debug_buf);
            } else {
                free(filtered);
                log_message("[DEBUG] Bbox filter removed all points");
            }
        }
    }
    
    // Simplificare optionala a traseului folosind algoritmul Douglas-Peucker
    if (task->epsilon > 0 && point_count > 2) {
        snprintf(debug_buf, sizeof(debug_buf), "[DEBUG] Applying Douglas-Peucker with epsilon=%.6f", task->epsilon);
        log_message(debug_buf);
        
        pointMsgType *simplified = NULL;
        int simplified_count = douglas_peucker(points, point_count, task->epsilon, &simplified);
        
        if (simplified_count > 0 && simplified_count < point_count) {
            free(points);
            points = simplified;
            point_count = simplified_count;
            snprintf(debug_buf, sizeof(debug_buf), "[DEBUG] After simplification: point_count=%d", point_count);
            log_message(debug_buf);
        } else if (simplified_count > 0) {
            free(simplified);
        }
    }
    
    // Verifică dacă avem suficiente puncte pentru a calcula distanțe
    if (point_count < 2) {
        log_message("[ERROR] Not enough points to calculate distances");
        if (points) free(points);
        task->points = NULL;
        task->status = 4;
        return;
    }
    
    // Calculeaza distanta totala si distantele pe fiecare segment consecutiv
    double total_distance = 0.0;
    int segment_count = point_count - 1;
    
    snprintf(debug_buf, sizeof(debug_buf), "[DEBUG] Calculating distances for %d points, %d segments", point_count, segment_count);
    log_message(debug_buf);
    
    // Alocare dinamica pentru segment_distances
    segment_distances = malloc(sizeof(double) * segment_count);
    if (!segment_distances) {
        log_message("[ERROR] Failed to allocate segment_distances");
        if (points) free(points);
        task->points = NULL;
        task->status = 4;
        return;
    }
    
    for (int i = 0; i < segment_count; i++) {
        double dist = haversine_distance(points[i], points[i+1]);
        segment_distances[i] = dist;
        total_distance += dist;
        snprintf(debug_buf, sizeof(debug_buf), "[DEBUG] Segment %d-%d: distance=%.6f km", i+1, i+2, dist);
        log_message(debug_buf);
    }
    snprintf(debug_buf, sizeof(debug_buf), "[DEBUG] Total distance: %.6f km", total_distance);
    log_message(debug_buf);
    
    // Calculeaza optional distanta directa si distanta pe ruta intre doua puncte cerute de client
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
        
        snprintf(debug_buf, sizeof(debug_buf), "[DEBUG] Distance request: idx1=%d, idx2=%d, direct=%.6f, route=%.6f", 
                 task->dist_idx1, task->dist_idx2, direct_distance, route_distance);
        log_message(debug_buf);
    }
    
    if (task->cancel_flag) {
        log_message("[CANCEL] Task cancelled, discarding results");
        free(segment_distances);
        if (points) free(points);
        task->points = NULL;
        task->status = 3;
        if (task->upload_path[0] != '\0') unlink(task->upload_path);
        return;
    }
    
    // Limitează numărul de segmente stocate în result la maximum 1000
    int stored_segments = segment_count;
    if (stored_segments > 1000) {
        stored_segments = 1000;
        char warnbuf[128];
        snprintf(warnbuf, sizeof(warnbuf), "[WARN] Truncating segments from %d to 1000", segment_count);
        log_message(warnbuf);
    }
    
    // Store results in task->result instead of sending via socket
    snprintf(task->result.total_distance, sizeof(task->result.total_distance), "%.6f", total_distance);
    snprintf(task->result.point_count, sizeof(task->result.point_count), "%d", point_count);
    snprintf(task->result.segment_count, sizeof(task->result.segment_count), "%d", segment_count);
    task->result.segment_cnt = stored_segments;
    for (int i = 0; i < stored_segments; i++) {
        snprintf(task->result.segment_distances[i], sizeof(task->result.segment_distances[i]), "%.6f", segment_distances[i]);
    }
    snprintf(task->result.direct_distance, sizeof(task->result.direct_distance), "%.6f", direct_distance);
    snprintf(task->result.route_distance, sizeof(task->result.route_distance), "%.6f", route_distance);
    snprintf(task->result.has_request, sizeof(task->result.has_request), "%d", has_distance_request);
    snprintf(task->result.show_segments, sizeof(task->result.show_segments), "%d", task->show_segments);
    task->result.ready = 1;
    
    char output_file[512];
    snprintf(output_file, sizeof(output_file), "processing/outgoing/task_%d_result.csv", task->task_id);
    
    snprintf(debug_buf, sizeof(debug_buf), "[DEBUG] Saving CSV to: %s, point_count=%d, points=%p", output_file, point_count, (void*)points);
    log_message(debug_buf);
    
    int out_fd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd >= 0) {
        // Scrie header CSV
        char header[] = "lat,lon,distance_to_next_km\n";
        write(out_fd, header, strlen(header));
        log_message("[DEBUG] CSV header written");
        
        // Scrie punctele procesate cu distanțele către următorul punct
        char line[512];
        int lines_written = 0;
        for (int i = 0; i < point_count - 1; i++) {
            snprintf(line, sizeof(line), "%.6f,%.6f,%.6f\n", 
                     points[i].lat, points[i].lon, segment_distances[i]);
            write(out_fd, line, strlen(line));
            lines_written++;
            snprintf(debug_buf, sizeof(debug_buf), "[DEBUG] CSV line %d: %.6f,%.6f,%.6f", i, points[i].lat, points[i].lon, segment_distances[i]);
            log_message(debug_buf);
        }
        // Ultimul punct (distanța 0)
        if (point_count > 0) {
            snprintf(line, sizeof(line), "%.6f,%.6f,0.0\n", 
                     points[point_count-1].lat, 
                     points[point_count-1].lon);
            write(out_fd, line, strlen(line));
            lines_written++;
            snprintf(debug_buf, sizeof(debug_buf), "[DEBUG] CSV last line: %.6f,%.6f,0.0", points[point_count-1].lat, points[point_count-1].lon);
            log_message(debug_buf);
        }
        close(out_fd);
        
        snprintf(debug_buf, sizeof(debug_buf), "[DEBUG] CSV saved: %d lines written to %s", lines_written, output_file);
        log_message(debug_buf);
        
        // Salvează calea în task și în result
        strncpy(task->output_path, output_file, sizeof(task->output_path)-1);
        strncpy(task->result.output_path, output_file, sizeof(task->result.output_path)-1);
        
        char logbuf[1024];
        snprintf(logbuf, sizeof(logbuf), "[OUTPUT] Saved result to %s", output_file);
        log_message(logbuf);
    } else {
        char errbuf[128];
        snprintf(errbuf, sizeof(errbuf), "[OUTPUT] Failed to create output file for task %d", task->task_id);
        log_message(errbuf);
    }

    
    // Actualizeaza statisticile globale ale serverului dupa procesare
    stats_add_processed(point_count, total_distance, task->filename);
    
    // Scrie un mesaj de log cu rezumatul task-ului procesat
    char logbuf[1024];
    snprintf(logbuf, sizeof(logbuf), "[GEO] Task %d: %d points, distance=%.2f km", 
             task->task_id, point_count, total_distance);
    log_message(logbuf);
    
    // Eliberează memoria
    free(segment_distances);
    free(points);
    task->points = NULL;
    
    snprintf(debug_buf, sizeof(debug_buf), "[DEBUG] process_task_real END for task_id=%d", task->task_id);
    log_message(debug_buf);
}

// Creeaza un task nou si il adauga in coada de procesare.
int queue_add_task_full(const char *filename, int client_id, int sock_fd, int point_count,
                        const char *bbox, double epsilon, int show_segments,
                        int dist_idx1, int dist_idx2, pointMsgType *points, int request_id) {
    queue_task_t *task = malloc(sizeof(queue_task_t));
    if (!task) return -1;
    
    // Initializare campuri de baza pentru task-ul nou
    task->task_id = next_task_id++;
    task->request_id = request_id;
    strncpy(task->filename, filename, sizeof(task->filename) - 1);
    task->filename[sizeof(task->filename) - 1] = '\0';
    task->cancel_flag = 0;
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
    task->status = 0;  // pending
    task->start_time = time(NULL);
    task->end_time = 0;
    task->points = points;
    task->point_count = point_count;
    task->next = NULL;
    memset(&task->result, 0, sizeof(task_result_t));
    
    // Adaugare thread-safe la sfarsitul cozii
    pthread_mutex_lock(&queue_mutex);
    if (queue_tail) {
        queue_tail->next = task;
        queue_tail = task;
    } else {
        queue_head = queue_tail = task;
    }
    
    // Notifica thread-ul procesor ca exista un task nou de preluat
    pthread_cond_signal(&queue_cond);
    notify_queue();  // Notifică și prin pipe
    pthread_mutex_unlock(&queue_mutex);
    
    // Mesaj de log pentru urmarirea task-urilor adaugate
    char logbuf[256];
    snprintf(logbuf, sizeof(logbuf), "[QUEUE] Task %d added: %s (%d points)", 
             task->task_id, filename, point_count);
    log_message(logbuf);
    
    return task->task_id;
}

// Varianta simplificata de adaugare in coada, folosita cand nu sunt furnizate toate detaliile.
int queue_add_task(const char *filename, int client_id) {
    return queue_add_task_full(filename, client_id, -1, 0, NULL, -1, 0, 0, 0, NULL, 0);
}

// Formateaza continutul curent al cozii intr-un buffer text pentru afisare.
void format_queue_response(char *buffer, size_t bufsize) {
    pthread_mutex_lock(&queue_mutex);
    buffer[0] = '\0';
    queue_task_t *curr = queue_head;
    int count = 0;
    
    if (curr == NULL) {
        snprintf(buffer, bufsize, "Coada este goala\n");
    } else {
        // Se afiseaza maximum 10 task-uri pentru a evita raspunsuri prea mari
        while (curr && count < 10) {
            char status_str[32];
            if (curr->status == 0) {
                snprintf(status_str, sizeof(status_str), "In asteptare");
            } else if (curr->status == 1) {
                snprintf(status_str, sizeof(status_str), "In procesare");
            } else if (curr->status == 2) {
                snprintf(status_str, sizeof(status_str), "Finalizat");
            } else {
                snprintf(status_str, sizeof(status_str), "Anulat");
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

// Cancel a task by ID (if still pending or processing)
int cancel_task(int task_id) {
    pthread_mutex_lock(&queue_mutex);
    queue_task_t *curr = queue_head;
    int found = 0;
    while (curr) {
        if (curr->task_id == task_id && curr->status < 2) {
            curr->status = 3;        // cancelled
            curr->cancel_flag = 1;   // <--- ADĂUGĂ ACEASTA LINIE
            found = 1;
            char logbuf[256];
            snprintf(logbuf, sizeof(logbuf), "[QUEUE] Task %d cancelled", task_id);
            log_message(logbuf);
            break;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&queue_mutex);
    
    if (!found) {
        pthread_mutex_lock(&completed_mutex);
        curr = completed_head;
        while (curr) {
            if (curr->task_id == task_id) {
                found = 0;
                break;
            }
            curr = curr->next;
        }
        pthread_mutex_unlock(&completed_mutex);
    }
    
    return found;
}

// Get task status as string
void get_task_status(int task_id, char *buf, size_t bufsize) {
    buf[0] = '\0';
    
    // Caută în coada activă
    pthread_mutex_lock(&queue_mutex);
    queue_task_t *curr = queue_head;
    while (curr) {
        if (curr->task_id == task_id) {
            const char *status_str = "unknown";
            if (curr->status == 0) status_str = "pending";
            else if (curr->status == 1) status_str = "processing";
            else if (curr->status == 2) status_str = "done";
            else if (curr->status == 3) status_str = "cancelled";
            snprintf(buf, bufsize, "Task %d: %s", task_id, status_str);
            pthread_mutex_unlock(&queue_mutex);
            return;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&queue_mutex);
    
    // Caută în lista de finalizate
    pthread_mutex_lock(&completed_mutex);
    curr = completed_head;
    while (curr) {
        if (curr->task_id == task_id) {
            char time_str[64];
            char *ctime_result = ctime(&curr->end_time);
            if (ctime_result) {
                strncpy(time_str, ctime_result, sizeof(time_str) - 1);
                time_str[sizeof(time_str) - 1] = '\0';
                size_t len = strlen(time_str);
                if (len > 0 && time_str[len - 1] == '\n') {
                    time_str[len - 1] = '\0';
                }
            } else {
                strcpy(time_str, "unknown time");
            }
            snprintf(buf, bufsize, "Task %d: done (completed at %s)", task_id, time_str);
            pthread_mutex_unlock(&completed_mutex);
            return;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&completed_mutex);
    
    snprintf(buf, bufsize, "Task %d not found", task_id);
}

// Retrieve result of a completed task
int get_task_result(int task_id, task_result_t *result) {
    // Caută în coada activă
    pthread_mutex_lock(&queue_mutex);
    queue_task_t *curr = queue_head;
    while (curr) {
        if (curr->task_id == task_id && curr->status == 2) {
            memcpy(result, &curr->result, sizeof(task_result_t));
            pthread_mutex_unlock(&queue_mutex);
            return 1;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&queue_mutex);
    
    // Caută în lista de finalizate
    pthread_mutex_lock(&completed_mutex);
    curr = completed_head;
    while (curr) {
        if (curr->task_id == task_id && curr->status == 2) {
            memcpy(result, &curr->result, sizeof(task_result_t));
            pthread_mutex_unlock(&completed_mutex);
            return 1;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&completed_mutex);
    
    return 0;
}

// Thread-ul care consuma task-uri din coada si le proceseaza unul cate unul.
void *queue_processor(void *arg) {
    (void)arg;
    char logbuf[128];
    char hist_entry[600];
    char dummy;
    
    // Obține descriptorul pipe-ului pentru citire
    int pipe_fd = get_notify_pipe_read_fd();
    if (pipe_fd < 0) {
        log_message("[QUEUE] No notify pipe available, using cond wait fallback");
        // Fallback la vechea metodă
        while (1) {
            pthread_mutex_lock(&queue_mutex);
            while (queue_head == NULL) {
                pthread_cond_wait(&queue_cond, &queue_mutex);
            }
            queue_task_t *task = queue_head;
            queue_head = queue_head->next;
            if (queue_head == NULL) queue_tail = NULL;
            pthread_mutex_unlock(&queue_mutex);
            
            // Procesare task...
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
            
            pthread_mutex_lock(&completed_mutex);
            task->next = NULL;
            if (completed_tail) {
                completed_tail->next = task;
                completed_tail = task;
            } else {
                completed_head = completed_tail = task;
            }
            pthread_mutex_unlock(&completed_mutex);
        }
        return NULL;
    }
    
    // Noua metodă cu pipe
    while (1) {
        // Așteaptă notificare pe pipe
        if (read(pipe_fd, &dummy, 1) < 0) {
            if (errno == EINTR) continue;
            log_message("[QUEUE] Pipe read error");
            continue;
        }
        
        // Verifică dacă există task-uri în coadă
        pthread_mutex_lock(&queue_mutex);
        if (queue_head == NULL) {
            pthread_mutex_unlock(&queue_mutex);
            continue;
        }
        
        // Extrage primul task din coadă
        queue_task_t *task = queue_head;
        queue_head = queue_head->next;
        if (queue_head == NULL) queue_tail = NULL;
        pthread_mutex_unlock(&queue_mutex);
        
        // Verifică dacă task-ul a fost anulat înainte de procesare
        if (task->cancel_flag) {
            snprintf(logbuf, sizeof(logbuf), "[QUEUE] Task %d cancelled before processing", task->task_id);
            log_message(logbuf);
            task->status = 3;
            task->end_time = time(NULL);
            
            // Curăță fișierul uploadat dacă există
            if (task->upload_path[0] != '\0') {
                unlink(task->upload_path);
            }
            
            pthread_mutex_lock(&completed_mutex);
            task->next = NULL;
            if (completed_tail) {
                completed_tail->next = task;
                completed_tail = task;
            } else {
                completed_head = completed_tail = task;
            }
            pthread_mutex_unlock(&completed_mutex);
            continue;
        }
        
        // Marcheaza task-ul ca fiind in procesare
        task->status = 1;
        snprintf(logbuf, sizeof(logbuf), "[QUEUE] Processing task %d (%d points)", 
                 task->task_id, task->point_count);
        log_message(logbuf);
        
        process_task_real(task);
        
        // Marcheaza task-ul ca finalizat si calculeaza durata executiei
        task->status = 2;
        task->end_time = time(NULL);
        
        long exec_time_ms = (task->end_time - task->start_time) * 1000;
        snprintf(logbuf, sizeof(logbuf), "[QUEUE] Task %d completed in %ld ms", 
                 task->task_id, exec_time_ms);
        log_message(logbuf);
        
        // Adauga task-ul finalizat in istoric
        snprintf(hist_entry, sizeof(hist_entry), "Task %d: %s (%d puncte)", 
                 task->task_id, task->filename, task->point_count);
        add_to_history(hist_entry, exec_time_ms);
        
        // MUTĂ ÎN LISTA DE FINALIZATE
        pthread_mutex_lock(&completed_mutex);
        task->next = NULL;
        if (completed_tail) {
            completed_tail->next = task;
            completed_tail = task;
        } else {
            completed_head = completed_tail = task;
        }
        pthread_mutex_unlock(&completed_mutex);
    }
    return NULL;
}

// Creeaza o sesiune noua pentru un utilizator autentificat.
int session_create(const char *username) {
    pthread_mutex_lock(&session_mutex);
    
    client_session_t *new_session = malloc(sizeof(client_session_t));
    if (!new_session) {
        pthread_mutex_unlock(&session_mutex);
        return -1;
    }
    
    // Initializeaza datele sesiunii si o adauga la inceputul listei de sesiuni active
    new_session->session_id = next_session_id++;
    strncpy(new_session->username, username, sizeof(new_session->username) - 1);
    new_session->authenticated = 1;
    new_session->login_time = time(NULL);
    new_session->last_activity = time(NULL);
    new_session->next = sessions;
    sessions = new_session;
    
    int session_id = new_session->session_id;
    pthread_mutex_unlock(&session_mutex);
    
    // Scrie in log crearea sesiunii
    char logbuf[256];
    snprintf(logbuf, sizeof(logbuf), "[SESSION] Created session %d for user %s", session_id, username);
    log_message(logbuf);
    
    return session_id;
}

// Verifica daca o sesiune exista si este autentificata.
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

// Elimina o sesiune din lista de sesiuni active.
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

// Actualizeaza momentul ultimei activitati pentru o sesiune existenta.
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

// Verifica user-ul si parola comparand cu intrarile din fisierul de parole.
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
    
    // Citeste fisierul caracter cu caracter si verifica perechile user parola
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
    
    // Logheaza rezultatul autentificarii
    char logbuf[256];
    snprintf(logbuf, sizeof(logbuf), "[AUTH] User %s auth result: %d (using %s)", user, found, passwd_file);
    log_message(logbuf);
    
    return found;
}

// Adauga un utilizator nou in fisierul de parole al serverului.
// Datele sunt salvate in format simplu: user parola.
int add_user_server(const char *user, const char *pass) {
    const char *passwd_file = get_password_file();
    
    // Deschide fisierul de parole pentru adaugare la final; daca nu exista, il creeaza
    int fd = open(passwd_file, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) {
        char logbuf[256];
        snprintf(logbuf, sizeof(logbuf), "[AUTH] Cannot open passwords file for writing: %s", passwd_file);
        log_message(logbuf);
        return 0;
    }
    
    // Construieste linia care va fi scrisa in fisier
    char line[256];
    int len = snprintf(line, sizeof(line), "%s %s\n", user, pass);
    write(fd, line, len);
    close(fd);
    
    // Logheaza adaugarea utilizatorului nou
    char logbuf[256];
    snprintf(logbuf, sizeof(logbuf), "[AUTH] New user registered: %s (in %s)", user, passwd_file);
    log_message(logbuf);
    
    return 1;
}

// Creste numarul de clienti activi din statisticile globale.
void stats_increment_clients(void) {
    pthread_mutex_lock(&stats_mutex);
    g_stats.active_clients++;
    pthread_mutex_unlock(&stats_mutex);
}

// Scade numarul de clienti activi, fara a cobori sub zero.
void stats_decrement_clients(void) {
    pthread_mutex_lock(&stats_mutex);
    if (g_stats.active_clients > 0) g_stats.active_clients--;
    pthread_mutex_unlock(&stats_mutex);
}

// Actualizeaza statisticile globale dupa procesarea unei cereri GEO.
void stats_add_processed(int points, double distance, const char *filename) {
    pthread_mutex_lock(&stats_mutex);
    g_stats.total_processed_points += points;
    g_stats.total_processed_distance += distance;
    strncpy(g_stats.last_upload, filename, sizeof(g_stats.last_upload) - 1);
    pthread_mutex_unlock(&stats_mutex);
}

// Creste numarul de procese active din statistici.
void stats_increment_processes(void) {
    pthread_mutex_lock(&stats_mutex);
    g_stats.active_processes++;
    pthread_mutex_unlock(&stats_mutex);
}

// Scade numarul de procese active, fara a cobori sub zero.
void stats_decrement_processes(void) {
    pthread_mutex_lock(&stats_mutex);
    if (g_stats.active_processes > 0) g_stats.active_processes--;
    pthread_mutex_unlock(&stats_mutex);
}

// Copiaza statisticile curente intr-o structura furnizata de apelant.
void get_stats(server_stats_t *stats) {
    pthread_mutex_lock(&stats_mutex);
    memcpy(stats, &g_stats, sizeof(server_stats_t));
    pthread_mutex_unlock(&stats_mutex);
}

// Formateaza lista sesiunilor active intr-un buffer text pentru afisare.
void format_sessions_response(char *buffer, size_t bufsize) {
    pthread_mutex_lock(&session_mutex);
    buffer[0] = '\0';
    client_session_t *curr = sessions;
    int count = 0;
    
    // Daca nu exista sesiuni active, se afiseaza un mesaj explicit
    if (curr == NULL) {
        snprintf(buffer, bufsize, "Nicio sesiune activa\n");
    } else {
        // Se afiseaza maximum 20 de sesiuni pentru a evita raspunsuri prea mari
        while (curr && count < 20) {
            char timebuf[64];
            struct tm *tm_info = localtime(&curr->login_time);
            strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm_info);
            
            // Durata sesiunii este calculata relativ la momentul curent
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

// Inchide fortat o sesiune activa dupa ID-ul ei.
// Intoarce 1 daca sesiunea a fost gasita si eliminata, altfel 0.
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
            
            // Logheaza inchiderea administrativa a sesiunii
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

// Adaugă un task pentru un fișier deja salvat pe disc (nu puncte în memorie)
int queue_add_task_file(const char *filename, const char *upload_path, int client_id, int sock_fd,
                        const char *bbox, double epsilon, int show_segments,
                        int dist_idx1, int dist_idx2, int request_id) {
    queue_task_t *task = malloc(sizeof(queue_task_t));
    if (!task) return -1;

    task->task_id = next_task_id++;
    task->request_id = request_id;
    strncpy(task->filename, filename, sizeof(task->filename)-1);
    task->filename[sizeof(task->filename)-1] = '\0';
    task->cancel_flag = 0;                        
    strncpy(task->upload_path, upload_path, sizeof(task->upload_path)-1);
    task->upload_path[sizeof(task->upload_path)-1] = '\0';

    if (bbox) {
        strncpy(task->bbox, bbox, sizeof(task->bbox)-1);
        task->bbox[sizeof(task->bbox)-1] = '\0';
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
    task->points = NULL;          // nu avem puncte în memorie
    task->point_count = 0;
    task->next = NULL;
    memset(&task->result, 0, sizeof(task_result_t));

    pthread_mutex_lock(&queue_mutex);
    if (queue_tail) {
        queue_tail->next = task;
        queue_tail = task;
    } else {
        queue_head = queue_tail = task;
    }
    pthread_cond_signal(&queue_cond);
    notify_queue();  // Notifică și prin pipe
    pthread_mutex_unlock(&queue_mutex);

    char logbuf[256];
    snprintf(logbuf, sizeof(logbuf), "[QUEUE] Task %d added (file): %s", task->task_id, filename);
    log_message(logbuf);

    return task->task_id;
}

// Funcții de parsare pentru server (copiate din inetclient.c)
int parse_csv(const char *filename, pointMsgType **points) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0) return -1;
    
    pointMsgType *temp = malloc(sizeof(pointMsgType) * 100000);
    if (!temp) {
        close(fd);
        return -1;
    }
    
    char buffer[4096];
    ssize_t bytes;
    int point_count = 0;
    char *line, *next;
    
    while ((bytes = read(fd, buffer, sizeof(buffer) - 1)) > 0 && point_count < 100000) {
        buffer[bytes] = '\0';
        line = buffer;
        
        while ((next = strchr(line, '\n')) != NULL && point_count < 100000) {
            *next = '\0';
            if (sscanf(line, "%lf,%lf", &temp[point_count].lat, &temp[point_count].lon) == 2) {
                point_count++;
            }
            line = next + 1;
        }
    }
    
    close(fd);
    
    if (point_count == 0) {
        free(temp);
        return -1;
    }
    
    *points = temp;
    return point_count;
}

int parse_gpx(const char *filename, pointMsgType **points) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0) return -1;
    
    pointMsgType *temp = malloc(sizeof(pointMsgType) * 100000);
    if (!temp) {
        close(fd);
        return -1;
    }
    
    char buffer[65536];
    ssize_t bytes = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    
    if (bytes <= 0) {
        free(temp);
        return -1;
    }
    buffer[bytes] = '\0';
    
    int point_count = 0;
    char *ptr = buffer;
    
    while ((ptr = strstr(ptr, "<trkpt")) != NULL && point_count < 100000) {
        char *lat_ptr = strstr(ptr, "lat=\"");
        char *lon_ptr = strstr(ptr, "lon=\"");
        
        if (lat_ptr && lon_ptr) {
            double lat, lon;
            sscanf(lat_ptr + 5, "%lf", &lat);
            sscanf(lon_ptr + 5, "%lf", &lon);
            
            temp[point_count].lat = lat;
            temp[point_count].lon = lon;
            point_count++;
        }
        ptr++;
    }
    
    if (point_count == 0) {
        free(temp);
        return -1;
    }
    
    *points = temp;
    return point_count;
}

int parse_geojson(const char *filename, pointMsgType **points) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0) return -1;
    
    pointMsgType *temp = malloc(sizeof(pointMsgType) * 100000);
    if (!temp) {
        close(fd);
        return -1;
    }
    
    char buffer[65536];
    ssize_t bytes = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    
    if (bytes <= 0) {
        free(temp);
        return -1;
    }
    buffer[bytes] = '\0';
    
    int point_count = 0;
    char *ptr = buffer;
    char *coords = strstr(ptr, "\"coordinates\"");
    
    if (coords) {
        char *start = strchr(coords, '[');
        if (start) {
            char *coord_ptr = start;
            while (*coord_ptr && point_count < 100000) {
                char *open_bracket = strchr(coord_ptr, '[');
                if (!open_bracket) break;
                
                char *close_bracket = strchr(open_bracket, ']');
                if (!close_bracket) break;
                
                char coord_str[256];
                int len = close_bracket - open_bracket - 1;
                
                if (len > 0 && len < (int)sizeof(coord_str)) {
                    strncpy(coord_str, open_bracket + 1, len);
                    coord_str[len] = '\0';
                    
                    double lon, lat;
                    if (sscanf(coord_str, "%lf , %lf", &lon, &lat) == 2) {
                        temp[point_count].lat = lat;
                        temp[point_count].lon = lon;
                        point_count++;
                    }
                }
                coord_ptr = close_bracket + 1;
            }
        }
    }
    
    if (point_count == 0) {
        free(temp);
        return -1;
    }
    
    *points = temp;
    return point_count;
}
