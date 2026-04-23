#include "../include/logging.h"   // Pentru logarea evenimentelor si erorilor serverului
#include "../include/proto.h"     // Pentru structuri si tipuri comune folosite in server
#include <fcntl.h>                // Pentru operatii pe fisiere si flag-uri precum open()
#include <stdlib.h>               // Pentru functii utilitare, inclusiv getenv()

// Structura globala care retine statistici despre activitatea serverului.
// Accesul concurent este protejat cu mutex.
static server_stats_t g_stats = {0, 0, 0, 0.0, ""};
static pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;

// Lista de sesiuni active ale clientilor conectati.
// next_session_id este folosit pentru generarea unui ID unic pentru fiecare sesiune noua.
static client_session_t *sessions = NULL;
static int next_session_id = 1000;
static pthread_mutex_t session_mutex = PTHREAD_MUTEX_INITIALIZER;

// Numarul maxim de comenzi pastrate in istoric.
#define MAX_HISTORY 100

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
typedef struct queue_task {
    int task_id;                // identificator unic al task-ului
    char filename[512];         // numele fisierului asociat cererii
    char bbox[128];             // bounding box optional pentru filtrare
    double epsilon;             // prag pentru simplificare geometrica
    int show_segments;          // flag pentru afisarea segmentelor
    int dist_idx1;              // primul index pentru cererea de distanta
    int dist_idx2;              // al doilea index pentru cererea de distanta
    int client_id;              // ID-ul clientului care a trimis cererea
    int sock_fd;                // socket-ul pe care se trimite raspunsul
    int status;                 // starea task-ului: in asteptare / in procesare / finalizat
    time_t start_time;          // momentul inceperii task-ului
    time_t end_time;            // momentul finalizarii task-ului
    pointMsgType *points;       // vectorul de puncte primit de la client
    int point_count;            // numarul de puncte din cerere
    struct queue_task *next;    // legatura spre urmatorul task din coada
} queue_task_t;

// Pointerii pentru inceputul si sfarsitul cozii de task-uri.
// queue_cond este folosit pentru a trezi thread-ul procesor cand apare un task nou.
static queue_task_t *queue_head = NULL;
static queue_task_t *queue_tail = NULL;
static int next_task_id = 1;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

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

// Proceseaza efectiv un task GEO extras din coada.
// Etapele posibile sunt: filtrare dupa bounding box, simplificare si calcul de distante.
static void process_task_real(queue_task_t *task) {
    pointMsgType *points = task->points;
    int point_count = task->point_count;
    
    // Filtrare optionala a punctelor daca task-ul contine un bounding box valid
    if (task->bbox[0] != '\0') {
        double min_lat, max_lat, min_lon, max_lon;
        if (sscanf(task->bbox, "%lf,%lf,%lf,%lf", &min_lat, &max_lat, &min_lon, &max_lon) == 4) {
            pointMsgType *filtered = malloc(sizeof(pointMsgType) * point_count);
            int filtered_count = 0;
            
            // Pastreaza doar punctele care se afla in interiorul zonei cerute
            for (int i = 0; i < point_count; i++) {
                if (points[i].lat >= min_lat && points[i].lat <= max_lat &&
                    points[i].lon >= min_lon && points[i].lon <= max_lon) {
                    filtered[filtered_count++] = points[i];
                }
            }
            
            // Daca au ramas puncte valide, se inlocuieste vectorul initial cu cel filtrat
            if (filtered_count > 0) {
                free(points);
                points = filtered;
                point_count = filtered_count;
            } else {
                free(filtered);
            }
        }
    }
    
    // Simplificare optionala a traseului folosind algoritmul Douglas-Peucker
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
    
    // Calculeaza distanta totala si distantele pe fiecare segment consecutiv
    double total_distance = 0.0;
    double segment_distances[1000];
    int segment_count = point_count - 1;
    
    for (int i = 0; i < point_count - 1; i++) {
        double dist = haversine_distance(points[i], points[i+1]);
        segment_distances[i] = dist;
        total_distance += dist;
    }
    
    // Calculeaza optional distanta directa si distanta pe ruta intre doua puncte cerute de client
    double direct_distance = 0.0;
    double route_distance = 0.0;
    int has_distance_request = (task->dist_idx1 > 0 && task->dist_idx2 > 0 && 
                                 task->dist_idx1 <= point_count && task->dist_idx2 <= point_count);
    
    if (has_distance_request) {
        int idx1 = task->dist_idx1 - 1;
        int idx2 = task->dist_idx2 - 1;
        
        // Distanta directa intre cele doua puncte selectate
        direct_distance = haversine_distance(points[idx1], points[idx2]);
        
        // Distanta pe traseu intre aceleasi doua puncte
        int start = (idx1 < idx2) ? idx1 : idx2;
        int end = (idx1 > idx2) ? idx1 : idx2;
        route_distance = 0.0;
        for (int i = start; i < end; i++) {
            route_distance += haversine_distance(points[i], points[i+1]);
        }
    }
    
    // Pregateste header-ul raspunsului care va fi trimis clientului
    msgHeaderType h;
    h.clientID = task->client_id;
    h.opID = OPR_UPLOAD_GEO;
    
    // Valorile numerice sunt convertite in string pentru a fi trimise prin protocolul existent
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
    
    // Trimite sumarul rezultatului catre client
    writeSingleString(task->sock_fd, h, total_dist_str);
    writeSingleString(task->sock_fd, h, point_cnt_str);
    writeSingleString(task->sock_fd, h, seg_cnt_str);
    
    // Trimite distantele individuale pentru fiecare segment
    for (int i = 0; i < segment_count; i++) {
        char seg_str[64];
        snprintf(seg_str, sizeof(seg_str), "%.6f", segment_distances[i]);
        writeSingleString(task->sock_fd, h, seg_str);
    }
    
    // Trimite si informatiile suplimentare privind cererea de distanta si afisarea segmentelor
    writeSingleString(task->sock_fd, h, direct_dist_str);
    writeSingleString(task->sock_fd, h, route_dist_str);
    writeSingleString(task->sock_fd, h, has_req_str);
    writeSingleString(task->sock_fd, h, show_seg_str);
    
    // Actualizeaza statisticile globale ale serverului dupa procesare
    stats_add_processed(point_count, total_distance, task->filename);
    
    // Scrie un mesaj de log cu rezumatul task-ului procesat
    char logbuf[256];
    snprintf(logbuf, sizeof(logbuf), "[GEO] Task %d: %d points, distance=%.2f km", 
             task->task_id, point_count, total_distance);
    log_message(logbuf);
    
    free(points);
}

// Creeaza un task nou si il adauga in coada de procesare.
int queue_add_task_full(const char *filename, int client_id, int sock_fd, int point_count,
                        const char *bbox, double epsilon, int show_segments,
                        int dist_idx1, int dist_idx2, pointMsgType *points) {
    queue_task_t *task = malloc(sizeof(queue_task_t));
    if (!task) return -1;
    
    // Initializare campuri de baza pentru task-ul nou
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
    return queue_add_task_full(filename, client_id, -1, 0, NULL, -1, 0, 0, 0, NULL);
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

// Thread-ul care consuma task-uri din coada si le proceseaza unul cate unul.
void *queue_processor(void *arg) {
    (void)arg;
    char logbuf[128];
    char hist_entry[600];
    
    while (1) {
        pthread_mutex_lock(&queue_mutex);
        
        // Asteapta pana cand exista cel putin un task disponibil
        while (queue_head == NULL) {
            pthread_cond_wait(&queue_cond, &queue_mutex);
        }
        
        // Extrage primul task din coada
        queue_task_t *task = queue_head;
        queue_head = queue_head->next;
        if (queue_head == NULL) queue_tail = NULL;
        
        pthread_mutex_unlock(&queue_mutex);
        
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
        
        free(task);
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