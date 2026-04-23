#include "../include/proto.h"
#include <fcntl.h>
#include <sys/wait.h>
#include <ctype.h>

#define MAX_INPUT 256
#define MAX_POINTS 100000

static char current_user[64] = "";
static int session_id = 0;
static int sock = -1;


void trim(char *s) {
    if (!s) return;
    char *start = s;
    while (*start == ' ' || *start == '\t' || *start == '\n') start++;
    if (*start == '\0') { *s = '\0'; return; }
    char *end = start + strlen(start) - 1;
    while (end > start && (*end == ' ' || *end == '\t' || *end == '\n')) end--;
    *(end + 1) = '\0';
    if (start != s) memmove(s, start, strlen(start) + 1);
}

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
    
    writeSingleString(sock, h, user);
    writeSingleString(sock, h, pass);
    
    msgIntType m;
    readSingleInt(sock, &m);
    
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
    
    writeSingleString(sock, h, user);
    writeSingleString(sock, h, pass);
    
    msgIntType m;
    readSingleInt(sock, &m);
    
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

char* read_single_string(int sock) {
    char header[12];
    int received = 0;
    while (received < 12) {
        int n = recv(sock, header + received, 12 - received, 0);
        if (n <= 0) return NULL;
        received += n;
    }
    
    char len_buf[4];
    received = 0;
    while (received < 4) {
        int n = recv(sock, len_buf + received, 4 - received, 0);
        if (n <= 0) return NULL;
        received += n;
    }
    int str_len = ntohl(*(int*)len_buf);
    
    if (str_len <= 0 || str_len > 65536) return NULL;
    
    char *str = malloc(str_len + 1);
    if (!str) return NULL;
    
    received = 0;
    while (received < str_len) {
        int n = recv(sock, str + received, str_len - received, 0);
        if (n <= 0) {
            free(str);
            return NULL;
        }
        received += n;
    }
    str[str_len] = '\0';
    
    return str;
}


int send_points_to_server(pointMsgType *points, int point_count, const char *filename, 
                          const char *bbox, double epsilon, int show_segments,
                          int dist_idx1, int dist_idx2) {
    msgHeaderType h;
    char buf[256];
    char bbox_str[128] = "";
    char epsilon_str[32] = "";
    char segments_str[8] = "";
    char dist1_str[16] = "", dist2_str[16] = "";
    
    h.clientID = session_id;
    h.opID = OPR_UPLOAD_GEO;
    
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
    
    char *total_dist_str = read_single_string(sock);
    char *point_cnt_resp = read_single_string(sock);
    char *seg_cnt_str = read_single_string(sock);
    
    if (!total_dist_str || !point_cnt_resp || !seg_cnt_str) {
        write(STDERR_FILENO, "Eroare la primirea rezultatelor\n", 32);
        return -1;
    }
    
    double total_distance = atof(total_dist_str);
    int point_count_resp = atoi(point_cnt_resp);
    int segment_count = atoi(seg_cnt_str);
    
    free(total_dist_str);
    free(point_cnt_resp);
    free(seg_cnt_str);
    
    double *segment_distances = NULL;
    if (segment_count > 0) {
        segment_distances = malloc(sizeof(double) * segment_count);
        for (int i = 0; i < segment_count; i++) {
            char *seg_str = read_single_string(sock);
            if (seg_str) {
                segment_distances[i] = atof(seg_str);
                free(seg_str);
            }
        }
    }
    
    char *direct_dist_str = read_single_string(sock);
    char *route_dist_str = read_single_string(sock);
    char *has_req_str = read_single_string(sock);
    char *show_seg_resp = read_single_string(sock);
    
    double direct_distance = direct_dist_str ? atof(direct_dist_str) : 0.0;
    double route_distance = route_dist_str ? atof(route_dist_str) : 0.0;
    int has_distance_request = has_req_str ? atoi(has_req_str) : 0;
    int show_segments_resp = show_seg_resp ? atoi(show_seg_resp) : 0;
    
    free(direct_dist_str);
    free(route_dist_str);
    free(has_req_str);
    free(show_seg_resp);
    
    snprintf(buf, sizeof(buf), "\n=== REZULTATE DE LA SERVER ===\n");
    write(STDOUT_FILENO, buf, strlen(buf));
    
    snprintf(buf, sizeof(buf), "Puncte: %d\n", point_count_resp);
    write(STDOUT_FILENO, buf, strlen(buf));
    
    snprintf(buf, sizeof(buf), "Distanta totala: %.2f km\n", total_distance);
    write(STDOUT_FILENO, buf, strlen(buf));
    
    if (show_segments_resp && segment_count > 0) {
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
    return 0;
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
    servername.sin_port = htons(18081);
    servername.sin_addr.s_addr = inet_addr("127.0.0.1");
    
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
        "  <lat,lon> [lat,lon ...]                            - Introducere directa puncte\n"
        "  help                                               - Acest mesaj\n"
        "  exit                                               - Iesire\n"
        "\nExemple:\n"
        "  upload test.csv\n"
        "  upload --bbox 44,48,20,30 test.csv\n"
        "  upload --simplify 0.5 test.csv\n"
        "  upload --segments test.csv\n"
        "  upload --distance 1,5 test.csv\n"
        "  44.4268,26.1025 45.7489,21.2087\n";
    write(STDOUT_FILENO, help, strlen(help));
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
            writeSingleInt(sock, h, 0);
            close(sock);
            break;
        }
        else if (strcmp(line, "help") == 0) {
            print_usage();
            continue;
        }
        else if (strncmp(line, "upload", 6) == 0) {
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
    char menu[] = "\n=== CLIENT GEOSPAȚIAL ===\n"
                  "1. Autentificare\n"
                  "2. Creare cont nou\n"
                  "3. Iesire\n"
                  "Alege: ";
    write(STDOUT_FILENO, menu, strlen(menu));
}


int main(void) {
    char input[16];
    int authenticated = 0;
    int choice;
    
    sock = connect_to_server();
    if (sock < 0) {
        write(STDERR_FILENO, "Nu se poate conecta la server!\n", 32);
        return 1;
    }
    
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