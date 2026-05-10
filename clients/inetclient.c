#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <ctype.h>
#include <pthread.h>
#include <sys/socket.h>      // pentru socket, connect, send, recv
#include <netinet/in.h>      // pentru struct sockaddr_in, htons
#include <arpa/inet.h>       // pentru inet_addr
#include <errno.h>
#include <sys/stat.h>        // pentru mkdir

#define MAX_INPUT 256
#define MAX_POINTS 100000
#define OPR_LOGIN  10
#define OPR_CHECK_TASK  20
#define OPR_REGISTER    11 
#define OPR_GET_RESULT  21

#define OPR_UPLOAD_GEO   1   // upload fisier geografic
#define OPR_GET_DISTANCE 2   // calcul distanta
#define OPR_GET_BBOX     3   // obtinere bounding box
#define OPR_SIMPLIFY     4   // simplificare traseu
#define OPR_GET_STATUS   5   // status server
#define OPR_BYE          6   // inchidere conexiune
#define OPR_CANCEL_TASK  32
#define OPR_UPLOAD_FILE  50
#define OPR_DOWNLOAD_FILE 51

// Date globale ale clientului curent
static char current_user[64] = "";   // numele utilizatorului autentificat
static int session_id = 0;           // ID-ul sesiunii active
static int sock = -1;                // socket-ul de comunicare cu serverul
static char server_host[64] = "127.0.0.1";  // Adresa serverului (poate fi suprascrisa)
static int server_port = 18081;            // Portul serverului (poate fi suprascris)

// Request ID generator
static int next_request_id = 1;
static pthread_mutex_t request_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct pointMsgType {
    double lat; // latitudine
    double lon; // longitudine
} pointMsgType;

typedef struct msgIntType {
    int msg;
} msgIntType;

typedef struct msgHeaderType {
    int msgSize;    // dimensiunea mesajului
    int clientID;   // ID-ul clientului
    int opID;       // codul operatiei
    int requestID;  // ID-ul cererii (pentru corelare)
} msgHeaderType;

// MODIFIED: writeSingleString with request_id (16 bytes header + 4 bytes length + content)
int writeSingleString(int sock, msgHeaderType h, char *str) {
    int str_len = strlen(str);
    int total_len = 16 + 4 + str_len;  // header 16 + length 4 + content
    char *buffer = malloc(total_len);
    if (!buffer) return -1;
    
    int *parts = (int*)buffer;
    parts[0] = htonl(total_len);
    parts[1] = htonl(h.clientID);
    parts[2] = htonl(h.opID);
    parts[3] = htonl(h.requestID);
    parts[4] = htonl(str_len);
    
    memcpy(buffer + 20, str, str_len);
    
    ssize_t sent = send(sock, buffer, total_len, 0);
    free(buffer);
    
    if (sent != total_len) return -1;
    return 0;
}

// MODIFIED: read_single_string - returns string and request_id
char* read_single_string(int sock, int *ret_request_id) {
    char header[16];
    ssize_t received = 0;
    
    while (received < 16) {
        ssize_t n = recv(sock, header + received, 16 - received, 0);
        if (n <= 0) return NULL;
        received += n;
    }
    
    // Extrage request_id din header (bytes 12-16)
    if (ret_request_id) {
        *ret_request_id = ntohl(*(int*)(header + 12));
    }
    
    char len_buf[4];
    received = 0;
    while (received < 4) {
        ssize_t n = recv(sock, len_buf + received, 4 - received, 0);
        if (n <= 0) return NULL;
        received += n;
    }
    
    int str_len = ntohl(*(int*)len_buf);
    if (str_len <= 0 || str_len > 65536) return NULL;
    
    char *str = malloc(str_len + 1);
    if (!str) return NULL;
    
    received = 0;
    while (received < str_len) {
        ssize_t n = recv(sock, str + received, str_len - received, 0);
        if (n <= 0) {
            free(str);
            return NULL;
        }
        received += n;
    }
    
    str[str_len] = '\0';
    return str;
}

// MODIFIED: readSingleInt - returns value and request_id
int readSingleInt(int sock, msgIntType *m, int *ret_request_id) {
    char buffer[20];
    ssize_t received = 0;
    
    while (received < 20) {
        ssize_t n = recv(sock, buffer + received, 20 - received, 0);
        if (n <= 0) {
            m->msg = -1;
            return -1;
        }
        received += n;
    }
    
    if (ret_request_id) {
        *ret_request_id = ntohl(((int*)buffer)[3]);  // request_id e al 4-lea int (offset 12-16)
    }
    m->msg = ntohl(((int*)buffer)[4]);  // ultimii 4 bytes sunt valoarea
    return 0;
}

// MODIFIED: writeSingleInt with request_id (16 bytes header + 4 bytes payload)
int writeSingleInt(int sock, msgHeaderType h, int value) {
    char buffer[20];  // 16 bytes header + 4 bytes payload
    int *parts = (int*)buffer;
    
    parts[0] = htonl(16);           // msgSize
    parts[1] = htonl(h.clientID);   // clientID
    parts[2] = htonl(h.opID);       // opID
    parts[3] = htonl(h.requestID);  // requestID
    parts[4] = htonl(value);        // payload
    
    ssize_t sent = send(sock, buffer, sizeof(buffer), 0);
    if (sent != sizeof(buffer)) return -1;
    return 0;
}

int get_next_request_id(void) {
    pthread_mutex_lock(&request_mutex);
    int id = next_request_id++;
    pthread_mutex_unlock(&request_mutex);
    return id;
}

// Elimina spatiile si newline-urile de la inceputul si sfarsitul unui string
void trim(char *s) {
    if (!s) return;
    char *start = s;
    
    while (*start == ' ' || *start == '\t' || *start == '\n') start++;
    
    if (*start == '\0') { 
        *s = '\0'; 
        return; 
    }
    
    char *end = start + strlen(start) - 1;
    while (end > start && (*end == ' ' || *end == '\t' || *end == '\n')) end--;
    
    *(end + 1) = '\0';
    
    if (start != s) memmove(s, start, strlen(start) + 1);
}

// Citeste o linie dintr-un descriptor de fisier (ex: stdin)
ssize_t read_line(int fd, char *buf, size_t size) {
    ssize_t i = 0;
    char c;
    
    while (i < (ssize_t)size - 1) {
        if (read(fd, &c, 1) <= 0) break;
        if (c == '\n') break;
        buf[i++] = c;
    }
    
    buf[i] = '\0';
    return i;
}

// Parseaza un fisier CSV de forma "lat,lon" pe fiecare linie
int parse_csv(const char *filename, pointMsgType **points) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0) return -1;
    
    pointMsgType *temp = malloc(sizeof(pointMsgType) * MAX_POINTS);
    if (!temp) {
        close(fd);
        return -1;
    }
    
    char buffer[4096];
    ssize_t bytes;
    int point_count = 0;
    char *line, *next;
    
    while ((bytes = read(fd, buffer, sizeof(buffer) - 1)) > 0 && point_count < MAX_POINTS) {
        buffer[bytes] = '\0';
        line = buffer;
        
        while ((next = strchr(line, '\n')) != NULL && point_count < MAX_POINTS) {
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
    
    pointMsgType *temp = malloc(sizeof(pointMsgType) * MAX_POINTS);
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
    
    while ((ptr = strstr(ptr, "<trkpt")) != NULL && point_count < MAX_POINTS) {
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
    
    pointMsgType *temp = malloc(sizeof(pointMsgType) * MAX_POINTS);
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
            while (*coord_ptr && point_count < MAX_POINTS) {
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

int do_login(int sock) {
    char user[64], pass[64];
    msgHeaderType h;
    
    write(STDOUT_FILENO, "Utilizator: ", 12);
    if (read_line(STDIN_FILENO, user, sizeof(user)) <= 0) return 0;
    trim(user);
    
    write(STDOUT_FILENO, "Parola: ", 8);
    if (read_line(STDIN_FILENO, pass, sizeof(pass)) <= 0) return 0;
    trim(pass);
    
    h.clientID = 0;
    h.opID = OPR_LOGIN;
    h.requestID = get_next_request_id();
    int sent_req = h.requestID;
    
    writeSingleString(sock, h, user);
    writeSingleString(sock, h, pass);
    
    msgIntType m;
    int recv_req;
    readSingleInt(sock, &m, &recv_req);
    
    if (recv_req != sent_req) {
        char warn[128];
        snprintf(warn, sizeof(warn), "Warning: request_id mismatch! Sent %d, got %d\n", sent_req, recv_req);
        write(STDERR_FILENO, warn, strlen(warn));
    }
    
    if (m.msg > 0) {
        session_id = m.msg;
        strncpy(current_user, user, sizeof(current_user) - 1);
        char buf[128];
        snprintf(buf, sizeof(buf), "Autentificare reusita! Session ID: %d\n", session_id);
        write(STDOUT_FILENO, buf, strlen(buf));
        return 1;
    }
    
    return 0;
}

int do_register(int sock) {
    char user[64], pass[64];
    msgHeaderType h;
    
    write(STDOUT_FILENO, "Nou utilizator: ", 16);
    if (read_line(STDIN_FILENO, user, sizeof(user)) <= 0) return 0;
    trim(user);
    
    write(STDOUT_FILENO, "Parola noua: ", 12);
    if (read_line(STDIN_FILENO, pass, sizeof(pass)) <= 0) return 0;
    trim(pass);
    
    h.clientID = 0;
    h.opID = OPR_REGISTER;
    h.requestID = get_next_request_id();
    int sent_req = h.requestID;
    
    writeSingleString(sock, h, user);
    writeSingleString(sock, h, pass);
    
    msgIntType m;
    int recv_req;
    readSingleInt(sock, &m, &recv_req);
    
    if (recv_req != sent_req) {
        char warn[128];
        snprintf(warn, sizeof(warn), "Warning: request_id mismatch! Sent %d, got %d\n", sent_req, recv_req);
        write(STDERR_FILENO, warn, strlen(warn));
    }
    
    if (m.msg > 0) {
        session_id = m.msg;
        strncpy(current_user, user, sizeof(current_user) - 1);
        write(STDOUT_FILENO, "Cont creat cu succes!\n", 22);
        char buf[128];
        snprintf(buf, sizeof(buf), "Session ID: %d\n", session_id);
        write(STDOUT_FILENO, buf, strlen(buf));
        return 1;
    }
    
    write(STDERR_FILENO, "Eroare la creare cont!\n", 23);
    return 0;
}

void check_task_status(int task_id) {
    msgHeaderType h;
    h.clientID = session_id;
    h.opID = OPR_CHECK_TASK;
    h.requestID = get_next_request_id();
    int sent_req = h.requestID;
    
    writeSingleInt(sock, h, task_id);
    
    int recv_req;
    char *status = read_single_string(sock, &recv_req);
    if (status) {
        if (recv_req != sent_req) {
            char warn[128];
            snprintf(warn, sizeof(warn), "Warning: request_id mismatch! Sent %d, got %d\n", sent_req, recv_req);
            write(STDERR_FILENO, warn, strlen(warn));
        }
        char buf[256];
        snprintf(buf, sizeof(buf), "%s\n", status);
        write(STDOUT_FILENO, buf, strlen(buf));
        free(status);
    } else {
        write(STDERR_FILENO, "Eroare la verificarea statusului task-ului\n", 42);
    }
}

void get_task_result(int task_id) {
    msgHeaderType h;
    h.clientID = session_id;
    h.opID = OPR_GET_RESULT;
    h.requestID = get_next_request_id();
    int sent_req = h.requestID;
    
    writeSingleInt(sock, h, task_id);
    
    int recv_req1, recv_req2, recv_req3;
    char *total_dist_str = read_single_string(sock, &recv_req1);
    char *point_cnt_resp = read_single_string(sock, &recv_req2);
    char *seg_cnt_str = read_single_string(sock, &recv_req3);
    
    if (!total_dist_str || !point_cnt_resp || !seg_cnt_str) {
        write(STDERR_FILENO, "Eroare la primirea rezultatelor\n", 32);
        if (total_dist_str) free(total_dist_str);
        if (point_cnt_resp) free(point_cnt_resp);
        if (seg_cnt_str) free(seg_cnt_str);
        return;
    }
    
    if (recv_req1 != sent_req || recv_req2 != sent_req || recv_req3 != sent_req) {
        char warn[128];
        snprintf(warn, sizeof(warn), "Warning: request_id mismatch in result!\n");
        write(STDERR_FILENO, warn, strlen(warn));
    }
    
    if (strcmp(total_dist_str, "ERROR: result not ready or task not found") == 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s\n", total_dist_str);
        write(STDOUT_FILENO, buf, strlen(buf));
        free(total_dist_str);
        free(point_cnt_resp);
        free(seg_cnt_str);
        return;
    }
    
    double total_distance = atof(total_dist_str);
    int point_count_resp = atoi(point_cnt_resp);
    int segment_count = atoi(seg_cnt_str);
    
    free(total_dist_str);
    free(point_cnt_resp);
    free(seg_cnt_str);
    
    double *segment_distances = NULL;
    if (segment_count > 0 && segment_count < 10000) {
        segment_distances = malloc(sizeof(double) * segment_count);
        for (int i = 0; i < segment_count; i++) {
            int recv_req;
            char *seg_str = read_single_string(sock, &recv_req);
            if (seg_str) {
                segment_distances[i] = atof(seg_str);
                free(seg_str);
            }
        }
    }
    
    int recv_req_dir, recv_req_route, recv_req_has, recv_req_show;
    char *direct_dist_str = read_single_string(sock, &recv_req_dir);
    char *route_dist_str = read_single_string(sock, &recv_req_route);
    char *has_req_str = read_single_string(sock, &recv_req_has);
    char *show_seg_resp = read_single_string(sock, &recv_req_show);
    
    double direct_distance = direct_dist_str ? atof(direct_dist_str) : 0.0;
    double route_distance = route_dist_str ? atof(route_dist_str) : 0.0;
    int has_distance_request = has_req_str ? atoi(has_req_str) : 0;
    int show_segments_resp = show_seg_resp ? atoi(show_seg_resp) : 0;
    
    free(direct_dist_str);
    free(route_dist_str);
    free(has_req_str);
    free(show_seg_resp);
    
    char buf[512];
    snprintf(buf, sizeof(buf), "\n=== REZULTATE DE LA SERVER (Task %d) ===\n", task_id);
    write(STDOUT_FILENO, buf, strlen(buf));
    snprintf(buf, sizeof(buf), "Puncte: %d\n", point_count_resp);
    write(STDOUT_FILENO, buf, strlen(buf));
    snprintf(buf, sizeof(buf), "Distanta totala: %.2f km\n", total_distance);
    write(STDOUT_FILENO, buf, strlen(buf));
    
    if (show_segments_resp && segment_count > 0 && segment_distances) {
        write(STDOUT_FILENO, "\n=== DISTANTE PE SEGMENTE ===\n", 30);
        double total = 0;
        for (int i = 0; i < segment_count; i++) {
            total += segment_distances[i];
            double pct = (total > 0) ? (segment_distances[i] / total) * 100 : 0;
            snprintf(buf, sizeof(buf), "Segment %d-%d: %.2f km (%.1f%%)\n", 
                     i+1, i+2, segment_distances[i], pct);
            write(STDOUT_FILENO, buf, strlen(buf));
        }
    }
    
    if (has_distance_request) {
        write(STDOUT_FILENO, "\n=== DISTANTA INTRE PUNCTE ===\n", 30);
        snprintf(buf, sizeof(buf), "Distanta directa: %.2f km\n", direct_distance);
        write(STDOUT_FILENO, buf, strlen(buf));
        snprintf(buf, sizeof(buf), "Distanta pe traseu: %.2f km\n", route_distance);
        write(STDOUT_FILENO, buf, strlen(buf));
    }
    
    if (segment_distances) free(segment_distances);
}

int send_points_to_server(pointMsgType *points, int point_count, const char *filename, 
                          const char *bbox, double epsilon, int show_segments,
                          int dist_idx1, int dist_idx2) {
    msgHeaderType h;
    char bbox_str[128] = "";
    char epsilon_str[32] = "";
    char segments_str[8] = "";
    char dist1_str[16] = "", dist2_str[16] = "";
    char buf[256];
    
    h.clientID = session_id;
    h.opID = OPR_UPLOAD_GEO;
    h.requestID = get_next_request_id();
    int sent_req = h.requestID;
    
    char fname[256];
    strncpy(fname, filename, sizeof(fname) - 1);
    writeSingleString(sock, h, fname);
    
    char point_cnt_str[32];
    snprintf(point_cnt_str, sizeof(point_cnt_str), "%d", point_count);
    writeSingleString(sock, h, point_cnt_str);
    
    if (bbox) {
        snprintf(bbox_str, sizeof(bbox_str), "%s", bbox);
    }
    writeSingleString(sock, h, bbox_str);
    
    if (epsilon > 0) {
        snprintf(epsilon_str, sizeof(epsilon_str), "%.6f", epsilon);
    }
    writeSingleString(sock, h, epsilon_str);
    
    snprintf(segments_str, sizeof(segments_str), "%d", show_segments);
    writeSingleString(sock, h, segments_str);
    
    snprintf(dist1_str, sizeof(dist1_str), "%d", dist_idx1);
    snprintf(dist2_str, sizeof(dist2_str), "%d", dist_idx2);
    writeSingleString(sock, h, dist1_str);
    writeSingleString(sock, h, dist2_str);
    
    for (int i = 0; i < point_count; i++) {
        char coord_str[64];
        snprintf(coord_str, sizeof(coord_str), "%.6f,%.6f", points[i].lat, points[i].lon);
        writeSingleString(sock, h, coord_str);
    }
    
    msgIntType task_id_msg;
    int recv_req;
    readSingleInt(sock, &task_id_msg, &recv_req);
    
    if (recv_req != sent_req) {
        char warn[128];
        snprintf(warn, sizeof(warn), "Warning: request_id mismatch! Sent %d, got %d\n", sent_req, recv_req);
        write(STDERR_FILENO, warn, strlen(warn));
    }
    
    int task_id = task_id_msg.msg;
    
    snprintf(buf, sizeof(buf), "\n=== UPLOAD INITIAT ===\nTask ID: %d\n", task_id);
    write(STDOUT_FILENO, buf, strlen(buf));
    write(STDOUT_FILENO, "Folositi 'status <id>' pentru a verifica progresul\n", 49);
    write(STDOUT_FILENO, "Folositi 'result <id>' pentru a obtine rezultatele cand task-ul este finalizat\n", 77);
    
    return task_id;
}

int upload_geo_file(const char *filename, const char *bbox, double epsilon, 
                    int show_segments, int dist_idx1, int dist_idx2) {
    int point_count = 0;
    pointMsgType *points = NULL;
    char buf[256];
    
    const char *ext = strrchr(filename, '.');
    if (ext) {
        if (strcmp(ext, ".gpx") == 0 || strcmp(ext, ".GPX") == 0) {
            point_count = parse_gpx(filename, &points);
        } else if (strcmp(ext, ".geojson") == 0 || strcmp(ext, ".json") == 0) {
            point_count = parse_geojson(filename, &points);
        } else {
            point_count = parse_csv(filename, &points);
        }
    } else {
        point_count = parse_csv(filename, &points);
    }
    
    if (point_count <= 0) {
        write(STDERR_FILENO, "Eroare: nu s-au putut citi punctele\n", 36);
        return -1;
    }
    
    snprintf(buf, sizeof(buf), "Au fost citite %d puncte\n", point_count);
    write(STDOUT_FILENO, buf, strlen(buf));
    
    return send_points_to_server(points, point_count, filename, bbox, epsilon, 
                                  show_segments, dist_idx1, dist_idx2);
}

void print_prompt(void) {
    char prompt[256];
    snprintf(prompt, sizeof(prompt), "\n%s@geoclient> ", current_user);
    write(STDOUT_FILENO, prompt, strlen(prompt));
}

int connect_to_server(void) {
    int s;
    struct sockaddr_in servername;
    
    s = socket(PF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    
    memset(&servername, 0, sizeof(servername));
    servername.sin_family = AF_INET;
    servername.sin_port = htons(server_port);
    servername.sin_addr.s_addr = inet_addr(server_host);
    
    if (connect(s, (struct sockaddr *)&servername, sizeof(servername)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

void print_usage(void) {
    char help[] = 
        "\nComenzi disponibile:\n"
        "  upload <fisier>                                    - Upload simplu\n"
        "  upload --bbox <min_lat,max_lat,min_lon,max_lon> <fisier>\n"
        "  upload --simplify <epsilon> <fisier>               - Simplificare traseu\n"
        "  upload --segments <fisier>                         - Afiseaza distante pe segmente\n"
        "  upload --distance <idx1,idx2> <fisier>             - Distanta intre doua puncte\n"
        "  status <task_id>                                   - Verifica statusul unui task\n"
        "  result <task_id>                                   - Obtine rezultatele unui task finalizat\n"
        "  cancel <task_id>                                   - Anuleaza un task (daca inca nu a fost procesat)\n"
        "  download <task_id>                                 - Descarca fisierul rezultat al unui task finalizat\n"
        "  <lat,lon> [lat,lon ...]                            - Introducere directa puncte\n"
        "  help                                               - Acest mesaj\n"
        "  exit                                               - Iesire\n"
        "\nExemple:\n"
        "  upload test.csv\n"
        "  upload --bbox 44,48,20,30 test.csv\n"
        "  upload --simplify 0.5 test.csv\n"
        "  upload --segments test.csv\n"
        "  upload --distance 1,5 test.csv\n"
        "  status 1\n"
        "  result 1\n"
        "  cancel 1\n"
        "  download 1\n"
        "  44.4268,26.1025 45.7489,21.2087\n";
    write(STDOUT_FILENO, help, strlen(help));
}

void cancel_task_client(int task_id) {
    msgHeaderType h;
    h.clientID = session_id;
    h.opID = OPR_CANCEL_TASK;
    h.requestID = get_next_request_id();
    int sent_req = h.requestID;
    
    writeSingleInt(sock, h, task_id);
    
    int recv_req;
    char *response = read_single_string(sock, &recv_req);
    if (response) {
        if (recv_req != sent_req) {
            char warn[128];
            snprintf(warn, sizeof(warn), "Warning: request_id mismatch! Sent %d, got %d\n", sent_req, recv_req);
            write(STDERR_FILENO, warn, strlen(warn));
        }
        char buf[256];
        snprintf(buf, sizeof(buf), "%s\n", response);
        write(STDOUT_FILENO, buf, strlen(buf));
        free(response);
    } else {
        write(STDERR_FILENO, "Eroare la anularea task-ului\n", 29);
    }
}


void download_task_result(int task_id) {
    msgHeaderType h;
    h.clientID = session_id;
    h.opID = OPR_DOWNLOAD_FILE;
    h.requestID = get_next_request_id();
    int sent_req = h.requestID;
    
    writeSingleInt(sock, h, task_id);
    
    // Primește numele fișierului
    int recv_req;
    char *filename = read_single_string(sock, &recv_req);
    if (!filename) {
        write(STDERR_FILENO, "Eroare la primirea numelui fisierului\n", 37);
        return;
    }
    
    if (recv_req != sent_req) {
        char warn[128];
        snprintf(warn, sizeof(warn), "Warning: request_id mismatch! Sent %d, got %d\n", sent_req, recv_req);
        write(STDERR_FILENO, warn, strlen(warn));
    }
    
    // Primește dimensiunea
    msgIntType size_msg;
    readSingleInt(sock, &size_msg, &recv_req);
    size_t file_size = size_msg.msg;
    
    // Creează directorul download/ dacă nu există
    mkdir("download", 0755);
    
    char output_path[512];
    snprintf(output_path, sizeof(output_path), "download/%s", filename);
    
    char dbg[256];
    snprintf(dbg, sizeof(dbg), "[CLIENT] Downloading file: %s, size: %zu bytes\n", filename, file_size);
    write(STDOUT_FILENO, dbg, strlen(dbg));
    
    int out_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        free(filename);
        write(STDERR_FILENO, "Eroare la crearea fisierului local\n", 35);
        return;
    }
    
    // Primește chunk-uri și scrie în fișier
    char chunk[8192];
    size_t received = 0;
    int error = 0;
    
    while (received < file_size) {
        size_t to_read = (file_size - received) < sizeof(chunk) ? (file_size - received) : sizeof(chunk);
        ssize_t n = recv(sock, chunk, to_read, 0);
        if (n <= 0) {
            error = 1;
            break;
        }
        ssize_t written = write(out_fd, chunk, n);
        if (written != n) {
            error = 1;
            break;
        }
        received += n;
    }
    close(out_fd);
    
    if (error || received != file_size) {
        unlink(output_path);
        char err[128];
        snprintf(err, sizeof(err), "Eroare la descarcare: primiti %zu din %zu bytes\n", received, file_size);
        write(STDERR_FILENO, err, strlen(err));
    } else {
        char buf[1024];
        snprintf(buf, sizeof(buf), "\n=== DOWNLOAD COMPLET ===\nFisier salvat: %s (%zu bytes)\n", output_path, file_size);
        write(STDOUT_FILENO, buf, strlen(buf));
    }
    
    free(filename);
}

int parse_points_from_args(char *line, pointMsgType **points) {
    pointMsgType *temp = malloc(sizeof(pointMsgType) * MAX_POINTS);
    if (!temp) return -1;
    
    int point_count = 0;
    char *token = strtok(line, " ");
    
    while (token != NULL && point_count < MAX_POINTS) {
        char *comma = strchr(token, ',');
        if (comma) {
            *comma = '\0';
            double lat = atof(token);
            double lon = atof(comma + 1);
            temp[point_count].lat = lat;
            temp[point_count].lon = lon;
            point_count++;
        }
        token = strtok(NULL, " ");
    }
    
    if (point_count == 0) {
        free(temp);
        return -1;
    }
    
    *points = temp;
    return point_count;
}

int upload_raw_file(const char *filename, const char *bbox, double epsilon,
                    int show_segments, int dist_idx1, int dist_idx2) {
    char dbg[256];
    
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        write(STDERR_FILENO, "Eroare: nu se poate deschide fisierul\n", 38);
        return -1;
    }
    
    off_t file_size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    
    snprintf(dbg, sizeof(dbg), "[CLIENT] File: %s, size: %ld bytes\n", filename, (long)file_size);
    write(STDOUT_FILENO, dbg, strlen(dbg));
    
    msgHeaderType h;
    h.clientID = session_id;
    h.opID = OPR_UPLOAD_FILE;
    h.requestID = get_next_request_id();
    int sent_req = h.requestID;
    
    write(STDOUT_FILENO, "[CLIENT] Sending filename...\n", 28);
    
    char fname[256];
    strncpy(fname, filename, sizeof(fname)-1);
    writeSingleString(sock, h, fname);
    
    write(STDOUT_FILENO, "[CLIENT] Sending file size...\n", 29);
    writeSingleInt(sock, h, (int)file_size);
    
    write(STDOUT_FILENO, "[CLIENT] Sending GEO params...\n", 30);
    
    char bbox_str[128] = "";
    if (bbox) snprintf(bbox_str, sizeof(bbox_str), "%s", bbox);
    writeSingleString(sock, h, bbox_str);
    
    char epsilon_str[32] = "";
    if (epsilon > 0) snprintf(epsilon_str, sizeof(epsilon_str), "%.6f", epsilon);
    writeSingleString(sock, h, epsilon_str);
    
    char segments_str[8] = "";
    snprintf(segments_str, sizeof(segments_str), "%d", show_segments);
    writeSingleString(sock, h, segments_str);
    
    char dist1_str[16] = "", dist2_str[16] = "";
    snprintf(dist1_str, sizeof(dist1_str), "%d", dist_idx1);
    snprintf(dist2_str, sizeof(dist2_str), "%d", dist_idx2);
    writeSingleString(sock, h, dist1_str);
    writeSingleString(sock, h, dist2_str);
    
    write(STDOUT_FILENO, "[CLIENT] GEO params sent, starting file transfer...\n", 50);
    
    char chunk[8192];
    ssize_t bytes;
    off_t total_sent = 0;
    int chunk_num = 0;
    
    while ((bytes = read(fd, chunk, sizeof(chunk))) > 0) {
        snprintf(dbg, sizeof(dbg), "[CLIENT] Sending chunk %d, size: %ld bytes, total sent: %ld/%ld\n", 
                 chunk_num++, (long)bytes, (long)total_sent, (long)file_size);
        write(STDOUT_FILENO, dbg, strlen(dbg));
        
        ssize_t sent = send(sock, chunk, bytes, 0);
        if (sent != bytes) {
            close(fd);
            write(STDERR_FILENO, "Eroare la trimiterea datelor\n", 29);
            return -1;
        }
        total_sent += sent;
    }
    close(fd);
    
    snprintf(dbg, sizeof(dbg), "[CLIENT] File transfer complete, total sent: %ld/%ld bytes\n", 
             (long)total_sent, (long)file_size);
    write(STDOUT_FILENO, dbg, strlen(dbg));
    
    if (total_sent != file_size) {
        write(STDERR_FILENO, "Eroare: dimensiune trimisa incorecta\n", 37);
        return -1;
    }
    
    write(STDOUT_FILENO, "[CLIENT] Waiting for task_id from server...\n", 43);
    
    msgIntType task_id_msg;
    int recv_req;
    readSingleInt(sock, &task_id_msg, &recv_req);
    
    if (recv_req != sent_req) {
        char warn[128];
        snprintf(warn, sizeof(warn), "Warning: request_id mismatch! Sent %d, got %d\n", sent_req, recv_req);
        write(STDERR_FILENO, warn, strlen(warn));
    }
    
    int task_id = task_id_msg.msg;
    
    snprintf(dbg, sizeof(dbg), "[CLIENT] Received task_id: %d\n", task_id);
    write(STDOUT_FILENO, dbg, strlen(dbg));
    
    char buf[256];
    snprintf(buf, sizeof(buf), "\n=== UPLOAD FILE INITIAT ===\nTask ID: %d\n", task_id);
    write(STDOUT_FILENO, buf, strlen(buf));
    write(STDOUT_FILENO, "Folositi 'status <id>' si 'result <id>' pentru a verifica\n", 57);
    
    return task_id;
}

void shell_loop(void) {
    char line[MAX_INPUT];
    
    while (1) {
        print_prompt();
        
        if (read_line(STDIN_FILENO, line, sizeof(line)) <= 0) break;
        trim(line);
        if (!*line) continue;
        
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
            msgHeaderType h;
            h.clientID = session_id;
            h.opID = OPR_BYE;
            h.requestID = get_next_request_id();
            writeSingleInt(sock, h, 0);
            close(sock);
            break;
        }
        else if (strcmp(line, "help") == 0) {
            print_usage();
            continue;
        }
        else if (strncmp(line, "status", 6) == 0) {
            char *cmd = strdup(line);
            char *args[10];
            int argc = 0;
            char *token = strtok(cmd, " ");
            while (token && argc < 10) {
                args[argc++] = token;
                token = strtok(NULL, " ");
            }
            if (argc == 2) {
                int task_id = atoi(args[1]);
                check_task_status(task_id);
            } else {
                write(STDERR_FILENO, "Folosire: status <task_id>\n", 27);
            }
            free(cmd);
        }
        else if (strncmp(line, "result", 6) == 0) {
            char *cmd = strdup(line);
            char *args[10];
            int argc = 0;
            char *token = strtok(cmd, " ");
            while (token && argc < 10) {
                args[argc++] = token;
                token = strtok(NULL, " ");
            }
            if (argc == 2) {
                int task_id = atoi(args[1]);
                get_task_result(task_id);
            } else {
                write(STDERR_FILENO, "Folosire: result <task_id>\n", 27);
            }
            free(cmd);
        }
        else if (strncmp(line, "cancel", 6) == 0) {
            char *cmd = strdup(line);
            char *args[10];
            int argc = 0;
            char *token = strtok(cmd, " ");
            while (token && argc < 10) {
                args[argc++] = token;
                token = strtok(NULL, " ");
            }
            if (argc == 2) {
                int task_id = atoi(args[1]);
                cancel_task_client(task_id);
            } else {
                write(STDERR_FILENO, "Folosire: cancel <task_id>\n", 27);
            }
            free(cmd);
        }

        else if (strncmp(line, "download", 8) == 0) {
            char *cmd = strdup(line);
            char *args[10];
            int argc = 0;
            char *token = strtok(cmd, " ");
            while (token && argc < 10) {
                args[argc++] = token;
                token = strtok(NULL, " ");
            }
            if (argc == 2) {
                int task_id = atoi(args[1]);
                download_task_result(task_id);
            } else {
                write(STDERR_FILENO, "Folosire: download <task_id>\n", 29);
            }
            free(cmd);
        }
        else if (strncmp(line, "upload", 6) == 0 && strncmp(line, "upload_raw", 10) != 0) {
            char *cmd = strdup(line);
            char *args[20];
            int argc = 0;
            char *token = strtok(cmd, " ");
            
            while (token && argc < 20) {
                args[argc++] = token;
                token = strtok(NULL, " ");
            }
            
            char *bbox = NULL;
            double epsilon = -1;
            int show_segments = 0;
            int dist_idx1 = 0, dist_idx2 = 0;
            char *filename = NULL;
            
            for (int i = 1; i < argc; i++) {
                if (strcmp(args[i], "--bbox") == 0 && i + 1 < argc) {
                    bbox = args[++i];
                } else if (strcmp(args[i], "--simplify") == 0 && i + 1 < argc) {
                    epsilon = atof(args[++i]);
                } else if (strcmp(args[i], "--segments") == 0) {
                    show_segments = 1;
                } else if (strcmp(args[i], "--distance") == 0 && i + 1 < argc) {
                    char *idx = args[++i];
                    sscanf(idx, "%d,%d", &dist_idx1, &dist_idx2);
                } else {
                    filename = args[i];
                }
            }
            
            if (filename) {
                upload_geo_file(filename, bbox, epsilon, show_segments, dist_idx1, dist_idx2);
            } else {
                write(STDERR_FILENO, "Specificati fisierul!\n", 22);
            }
            free(cmd);
        }
        else if (strncmp(line, "upload_raw", 10) == 0) {
            char *cmd = strdup(line);
            char *args[20];
            int argc = 0;
            char *token = strtok(cmd, " ");
            
            while (token && argc < 20) {
                args[argc++] = token;
                token = strtok(NULL, " ");
            }
            
            char *bbox = NULL;
            double epsilon = -1;
            int show_segments = 0;
            int dist_idx1 = 0, dist_idx2 = 0;
            char *filename = NULL;
            
            for (int i = 1; i < argc; i++) {
                if (strcmp(args[i], "--bbox") == 0 && i + 1 < argc) {
                    bbox = args[++i];
                } else if (strcmp(args[i], "--simplify") == 0 && i + 1 < argc) {
                    epsilon = atof(args[++i]);
                } else if (strcmp(args[i], "--segments") == 0) {
                    show_segments = 1;
                } else if (strcmp(args[i], "--distance") == 0 && i + 1 < argc) {
                    char *idx = args[++i];
                    sscanf(idx, "%d,%d", &dist_idx1, &dist_idx2);
                } else {
                    filename = args[i];
                }
            }
            
            if (filename) {
                upload_raw_file(filename, bbox, epsilon, show_segments, dist_idx1, dist_idx2);
            } else {
                write(STDERR_FILENO, "Specificati fisierul!\n", 22);
            }
            free(cmd);
        }
        else {
            pointMsgType *points = NULL;
            int point_count = parse_points_from_args(line, &points);
            
            if (point_count <= 0) {
                char err[] = "Comanda necunoscuta. Folositi 'help' pentru ajutor.\n";
                write(STDERR_FILENO, err, strlen(err));
                continue;
            }
            
            char buf[64];
            snprintf(buf, sizeof(buf), "Au fost citite %d puncte\n", point_count);
            write(STDOUT_FILENO, buf, strlen(buf));
            
            send_points_to_server(points, point_count, "manual_input", NULL, -1, 0, 0, 0);
            free(points);
        }
    }
}

void print_menu(void) {
    char menu[] = "\n=== CLIENT GEOSPATIAL ===\n"
                  "1. Autentificare\n"
                  "2. Creare cont nou\n"
                  "3. Iesire\n"
                  "Alege: ";
    write(STDOUT_FILENO, menu, strlen(menu));
}

void print_instructions(void) {
    char instructions[] = 
        "\n=== CLIENT GEOSPAȚIAL ===\n"
        "Utilizare: ./clients/inetclient [server_ip] [port]\n"
        "  server_ip - adresa IP a serverului (implicit: 127.0.0.1)\n"
        "  port      - portul serverului (implicit: 18081)\n"
        "\nExemple:\n"
        "  ./clients/inetclient               # Conectare la localhost:18081\n"
        "  ./clients/inetclient 192.168.1.100 # Conectare la 192.168.1.100:18081\n"
        "  ./clients/inetclient 10.0.0.1 8080 # Conectare la 10.0.0.1:8080\n\n";
    write(STDOUT_FILENO, instructions, strlen(instructions));
}

int main(int argc, char *argv[]) {
    char input[16];
    int authenticated = 0;
    int choice;
    
    if (argc > 1) {
        strncpy(server_host, argv[1], sizeof(server_host) - 1);
        server_host[sizeof(server_host) - 1] = '\0';
    }
    if (argc > 2) {
        server_port = atoi(argv[2]);
        if (server_port <= 0 || server_port > 65535) {
            write(STDERR_FILENO, "Port invalid. Folositi un port intre 1 si 65535.\n", 48);
            return 1;
        }
    }
    
    print_instructions();
    
    char conn_info[256];
    snprintf(conn_info, sizeof(conn_info), "Conectare la %s:%d...\n", server_host, server_port);
    write(STDOUT_FILENO, conn_info, strlen(conn_info));
    
    sock = connect_to_server();
    if (sock < 0) {
        write(STDERR_FILENO, "Nu se poate conecta la server!\n", 32);
        return 1;
    }
    
    write(STDOUT_FILENO, "Conexiune stabilita cu succes!\n", 31);
    
    while (!authenticated) {
        print_menu();
        
        if (read_line(STDIN_FILENO, input, sizeof(input)) <= 0) continue;
        choice = atoi(input);
        
        switch (choice) {
            case 1:
                if (do_login(sock)) {
                    authenticated = 1;
                    write(STDOUT_FILENO, "\nBun venit, ", 12);
                    write(STDOUT_FILENO, current_user, strlen(current_user));
                    write(STDOUT_FILENO, "!\n", 2);
                    print_usage();
                } else {
                    write(STDERR_FILENO, "Autentificare esuata!\n", 22);
                }
                break;
            case 2:
                do_register(sock);
                break;
            case 3:
                close(sock);
                write(STDOUT_FILENO, "La revedere!\n", 13);
                return 0;
            default:
                write(STDERR_FILENO, "Optiune invalida!\n", 18);
                break;
        }
    }
    
    shell_loop();
    
    char bye[] = "\nClient inchis.\n";
    write(STDOUT_FILENO, bye, sizeof(bye)-1);
    return 0;
}