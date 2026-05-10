#include "../include/logging.h"   // Pentru logarea evenimentelor si erorilor serverului
#include "../include/proto.h"     // Pentru structurile de mesaje, constantele OPR_* si functiile de comunicare
#include "../include/config.h"    // Pentru acces la configuratia globala a serverului
#include <fcntl.h>                // Pentru constante si operatii asociate fisierelor
#include <stdlib.h>               // Pentru malloc(), free(), atoi() si atof()
#include <netdb.h>
// Declaratii externe pentru functiile implementate in alte module.
// Acestea sunt folosite aici pentru autentificare, sesiuni, statistici si coada de task-uri.
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
                                pointMsgType *points, int request_id);
extern int blacklist_check(const char *ip);
extern void get_task_status(int task_id, char *buf, size_t bufsize);
extern int get_task_result(int task_id, task_result_t *result);
extern int cancel_task(int task_id);
extern int queue_add_task_file(const char *filename, const char *upload_path, int client_id, int sock_fd,
                                const char *bbox, double epsilon, int show_segments,
                                int dist_idx1, int dist_idx2, int request_id);
extern int domain_blacklist_check(const char *domain);
extern queue_task_t *completed_head;  // pentru a accesa lista de task-uri finalizate
extern pthread_mutex_t completed_mutex;

// Creeaza un socket TCP/IP, il configureaza pe portul primit si face bind pe toate interfetele.
// Parametrul reuse permite reutilizarea rapida a adresei dupa restart.
int inet_socket(uint16_t port, short reuse) {
    int sock;
    struct sockaddr_in name;
    
    // Creeaza socket-ul pentru comunicatie IPv4 orientata pe conexiune
    sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    
    // Activeaza optiunea SO_REUSEADDR daca este ceruta
    if (reuse) {
        int reuseAddrON = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuseAddrON, sizeof(reuseAddrON));
    }
    
    // Configureaza adresa locala pe care va asculta serverul
    name.sin_family = AF_INET;
    name.sin_port = htons(port);
    name.sin_addr.s_addr = htonl(INADDR_ANY);
    
    // Asociaza socket-ul cu portul si adresa configurate
    if (bind(sock, (struct sockaddr *)&name, sizeof(name)) < 0) {
        close(sock);
        return -1;
    }
    
    return sock;
}

// Thread-ul principal pentru serverul INET.
// Gestioneaza conexiuni noi si comenzile trimise de clientii conectati.
void *inet_main(void *args) {
    int port = *((int *)args);
    int sock;
    size_t size;
    fd_set active_fd_set, read_fd_set;
    struct sockaddr_in clientname;
    char logbuf[512];
    
    // Afiseaza un mesaj de pornire pentru serverul TCP/IP
    snprintf(logbuf, sizeof(logbuf), "[INET] Starting INET server on port %d...", port);
    write(STDOUT_FILENO, logbuf, strlen(logbuf));
    write(STDOUT_FILENO, "\n", 1);
    
    // Creeaza socket-ul de ascultare
    sock = inet_socket(port, 1);
    if (sock < 0) {
        write(STDOUT_FILENO, "[INET] Failed to create socket\n", 32);
        pthread_exit(NULL);
    }
    
    // Trecerea socket-ului in stare de listen pentru clienti multipli
    if (listen(sock, g_config.max_clients) < 0) {
        write(STDOUT_FILENO, "[INET] Failed to listen\n", 24);
        pthread_exit(NULL);
    }
    
    // Initializarea multimii de descriptori monitorizati cu select()
    FD_ZERO(&active_fd_set);
    FD_SET(sock, &active_fd_set);
    
    // Bucla principala a serverului
    while (1) {
        int i;
        read_fd_set = active_fd_set;
        struct timeval tv;
        
        // Seteaza timeout pentru select (1 secunda)
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        // Asteapta activitate pe oricare dintre socket-urile monitorizate
        if (select(FD_SETSIZE, &read_fd_set, NULL, NULL, &tv) < 0) {
            continue;
        }
        
        // Verifica fiecare descriptor care a devenit activ
        for (i = 0; i < FD_SETSIZE; ++i) {
            if (FD_ISSET(i, &read_fd_set)) {
                if (i == sock) {
                    int new_fd;
size = sizeof(clientname);
new_fd = accept(sock, (struct sockaddr *)&clientname, (socklen_t *)&size);
if (new_fd >= 0) {

    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    char client_ip[INET_ADDRSTRLEN];
    getpeername(new_fd, (struct sockaddr*)&client_addr, &addr_len);
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    if (blacklist_check(client_ip)) {
        write(new_fd, "Your IP is blacklisted.\n", 24);
        close(new_fd);
        continue;
    }
    
 
    char client_domain[256] = "";
    struct hostent *he = gethostbyaddr(&client_addr.sin_addr, sizeof(client_addr.sin_addr), AF_INET);
    if (he) {
        strncpy(client_domain, he->h_name, sizeof(client_domain)-1);
        client_domain[sizeof(client_domain)-1] = '\0';
        if (domain_blacklist_check(client_domain)) {
            write(new_fd, "Your domain is blacklisted.\n", 28);
            close(new_fd);
            continue;
        }
    }

    
    FD_SET(new_fd, &active_fd_set);
    stats_increment_clients();
    snprintf(logbuf, sizeof(logbuf), "[INET] New connection on fd=%d", new_fd);
    write(STDOUT_FILENO, logbuf, strlen(logbuf));
    write(STDOUT_FILENO, "\n", 1);
}
                } else {
                    // Cazul in care descriptorul activ apartine unui client deja conectat
                    msgHeaderType h = peekMsgHeader(i);
                    int operation = h.opID;
                    int client_id = h.clientID;
                    int request_id = h.requestID;
                    
                    // Daca peekMsgHeader a returnat valori invalide, clientul s-a deconectat
                    if (operation == -1 || client_id == -1) {
                        close(i);
                        FD_CLR(i, &active_fd_set);
                        stats_decrement_clients();
                        snprintf(logbuf, sizeof(logbuf), "[INET] Client disconnected (fd=%d)", i);
                        write(STDOUT_FILENO, logbuf, strlen(logbuf));
                        write(STDOUT_FILENO, "\n", 1);
                        continue;
                    }

                    
                    // Client nou fara ID valid: serverul ii aloca unul
                    if (operation == 0 && client_id == 0) {
                        msgIntType dummy;
                        if (readSingleInt(i, &dummy) < 0) {
                            // Eroare la citire, inchide conexiunea
                            close(i);
                            FD_CLR(i, &active_fd_set);
                            stats_decrement_clients();
                            continue;
                        }
                        int new_id = (int)time(NULL);
                        writeSingleInt(i, h, new_id);
                        snprintf(logbuf, sizeof(logbuf), "[INET] New client assigned ID=%d", new_id);
                        write(STDOUT_FILENO, logbuf, strlen(logbuf));
                        write(STDOUT_FILENO, "\n", 1);
                    }
                    else if (operation == OPR_LOGIN) {
                        // Tratarea cererii de autentificare
                        msgStringType user, pass;
                        if (readSingleString(i, &user) < 0 || readSingleString(i, &pass) < 0) {
                            char err_msg[] = "Eroare la autentificare";
                            writeSingleString(i, h, err_msg);
                            continue;
                        }
                        
                        int auth_result = authenticate_user(user.msg, pass.msg);
                        if (auth_result) {
                            // Daca autentificarea reuseste, se creeaza o sesiune noua
                            int session_id = session_create(user.msg);
                            session_sock_add(session_id, i);
                            writeSingleInt(i, h, session_id);
                            snprintf(logbuf, sizeof(logbuf), "[INET] User %s authenticated, session=%d", user.msg, session_id);
                            write(STDOUT_FILENO, logbuf, strlen(logbuf));
                            write(STDOUT_FILENO, "\n", 1);
                            
                            // Se salveaza actiunea in istoric
                            char hist_entry[256];
                            snprintf(hist_entry, sizeof(hist_entry), "Login: %s", user.msg);
                            add_to_history(hist_entry);
                        } else {
                            // Daca autentificarea esueaza, se trimite 0 ca ID de sesiune
                            writeSingleInt(i, h, 0);
                            snprintf(logbuf, sizeof(logbuf), "[INET] Auth failed for user %s", user.msg);
                            write(STDOUT_FILENO, logbuf, strlen(logbuf));
                            write(STDOUT_FILENO, "\n", 1);
                        }
                        
                        // Eliberarea memoriei alocate pentru stringurile citite
                        free(user.msg);
                        free(pass.msg);
                    }
                    else if (operation == OPR_REGISTER) {
                        // Tratarea cererii de inregistrare utilizator nou
                        msgStringType user, pass;
                        if (readSingleString(i, &user) < 0 || readSingleString(i, &pass) < 0) {
                            char err_msg[] = "Eroare la inregistrare";
                            writeSingleString(i, h, err_msg);
                            continue;
                        }
                        
                        int reg_result = add_user_server(user.msg, pass.msg);
                        if (reg_result) {
                            // Dupa inregistrare reusita, utilizatorul primeste si o sesiune
                            int session_id = session_create(user.msg);
                            writeSingleInt(i, h, session_id);
                            snprintf(logbuf, sizeof(logbuf), "[INET] New user registered: %s, session=%d", user.msg, session_id);
                            write(STDOUT_FILENO, logbuf, strlen(logbuf));
                            write(STDOUT_FILENO, "\n", 1);
                            
                            // Se adauga evenimentul in istoric
                            char hist_entry[256];
                            snprintf(hist_entry, sizeof(hist_entry), "Register: %s", user.msg);
                            add_to_history(hist_entry);
                        } else {
                            // Inregistrarea a esuat
                            writeSingleInt(i, h, 0);
                            write(STDOUT_FILENO, "[INET] Registration failed\n", 28);
                        }
                        
                        free(user.msg);
                        free(pass.msg);
                    }
                    else if (operation == OPR_UPLOAD_GEO) {
                        // Tratarea cererii de upload pentru date geografice
                        int session_id = client_id;
                        
                        // Verifica daca sesiunea clientului este valida
                        if (!session_validate(session_id)) {
                            char err_msg[] = "Sesiune invalida";
                            writeSingleString(i, h, err_msg);
                            continue;
                        }
                        
                        // Actualizeaza momentul ultimei activitati pentru sesiune
                        session_update_activity(session_id);
                        
                        // Citeste numele fisierului asociat upload-ului
                        msgStringType filename;
                        if (readSingleString(i, &filename) < 0) {
                            char err_msg[] = "Eroare la citirea numelui fisierului";
                            writeSingleString(i, h, err_msg);
                            continue;
                        }
                        
                        // Citeste numarul de puncte trimise de client
                        msgStringType point_count_str;
                        if (readSingleString(i, &point_count_str) < 0) {
                            free(filename.msg);
                            char err_msg[] = "Eroare la citirea numarului de puncte";
                            writeSingleString(i, h, err_msg);
                            continue;
                        }
                        int point_count = atoi(point_count_str.msg);
                        free(point_count_str.msg);
                        
                        // Validare simpla pentru numarul de puncte
                        if (point_count <= 0 || point_count > 100000) {
                            free(filename.msg);
                            char err_msg[] = "Numar invalid de puncte";
                            writeSingleString(i, h, err_msg);
                            continue;
                        }
                        
                        // Citeste optional bounding box-ul cerut de client
                        msgStringType bbox_str;
                        char bbox[128] = "";
                        if (readSingleString(i, &bbox_str) >= 0 && strlen(bbox_str.msg) > 0) {
                            strncpy(bbox, bbox_str.msg, sizeof(bbox) - 1);
                            free(bbox_str.msg);
                        }
                        
                        // Citeste optional epsilon pentru simplificare
                        msgStringType epsilon_str;
                        double epsilon = -1;
                        if (readSingleString(i, &epsilon_str) >= 0 && strlen(epsilon_str.msg) > 0) {
                            epsilon = atof(epsilon_str.msg);
                            free(epsilon_str.msg);
                        }
                        
                        // Citeste flag-ul care indica daca trebuie afisate segmentele
                        msgStringType segments_flag_str;
                        int show_segments = 0;
                        if (readSingleString(i, &segments_flag_str) >= 0 && strlen(segments_flag_str.msg) > 0) {
                            show_segments = atoi(segments_flag_str.msg);
                            free(segments_flag_str.msg);
                        }
                        
                        // Citeste optional doi indici folositi pentru calculul unei distante punct-la-punct
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
                        
                        // Aloca memorie pentru toate punctele primite de la client
                        pointMsgType *points = malloc(sizeof(pointMsgType) * point_count);
                        if (!points) {
                            free(filename.msg);
                            char err_msg[] = "Eroare de memorie";
                            writeSingleString(i, h, err_msg);
                            continue;
                        }
                        
                        // Citeste fiecare punct transmis sub forma de string "lat,lon"
                        int points_read = 0;
                        for (int p = 0; p < point_count; p++) {
                            msgStringType coord_str;
                            if (readSingleString(i, &coord_str) < 0) break;
                            sscanf(coord_str.msg, "%lf,%lf", &points[p].lat, &points[p].lon);
                            free(coord_str.msg);
                            points_read++;
                        }
                        
                        // Daca nu s-au citit toate punctele, upload-ul este considerat invalid
                        if (points_read != point_count) {
                            free(points);
                            free(filename.msg);
                            char err_msg[] = "Eroare la citirea punctelor";
                            writeSingleString(i, h, err_msg);
                            continue;
                        }
                        

                        int task_id = queue_add_task_full(filename.msg, session_id, i, point_count,
                                                          bbox, epsilon, show_segments,
                                                          dist_idx1, dist_idx2, points, request_id);
                        
                        // Send task_id back to client immediately
                        writeSingleInt(i, h, task_id);
                        
                        // Numele fisierului nu mai este necesar local dupa adaugarea task-ului
                        free(filename.msg);
                        
                        // Salveaza operatia in istoric
                        char hist_entry[256];
                        snprintf(hist_entry, sizeof(hist_entry), "Upload: %s (%d puncte) -> task %d", 
                                 filename.msg, point_count, task_id);
                        add_to_history(hist_entry);
                      
                    }

else if (operation == OPR_CHECK_TASK) {
    msgIntType task_id_msg;
    if (readSingleInt(i, &task_id_msg) < 0) {
        close(i);
        FD_CLR(i, &active_fd_set);
        stats_decrement_clients();
        continue;
    }
    int task_id = task_id_msg.msg;
    char status_buf[256];
    get_task_status(task_id, status_buf, sizeof(status_buf));
    
    // DEBUG: Verifică ce este în status_buf
    char dbg[512];
    snprintf(dbg, sizeof(dbg), "[DEBUG] status_buf content: '%s', length: %zu\n", status_buf, strlen(status_buf));
    write(STDOUT_FILENO, dbg, strlen(dbg));
    
    int ret = writeSingleString(i, h, status_buf);
    snprintf(dbg, sizeof(dbg), "[DEBUG] writeSingleString returned %d\n", ret);
    write(STDOUT_FILENO, dbg, strlen(dbg));
}

                    else if (operation == OPR_GET_RESULT) {
                        msgIntType task_id_msg;
                        if (readSingleInt(i, &task_id_msg) < 0) {
                            // Eroare la citire, inchide conexiunea
                            close(i);
                            FD_CLR(i, &active_fd_set);
                            stats_decrement_clients();
                            continue;
                        }
                        task_result_t res;
                        if (get_task_result(task_id_msg.msg, &res)) {
                            writeSingleString(i, h, res.total_distance);
                            writeSingleString(i, h, res.point_count);
                            writeSingleString(i, h, res.segment_count);
                            for (int j = 0; j < res.segment_cnt; j++)
                                writeSingleString(i, h, res.segment_distances[j]);
                            writeSingleString(i, h, res.direct_distance);
                            writeSingleString(i, h, res.route_distance);
                            writeSingleString(i, h, res.has_request);
                            writeSingleString(i, h, res.show_segments);
                        } else {
                            writeSingleString(i, h, "ERROR: result not ready or task not found");
                        }
                    }

                    else if (operation == OPR_CANCEL_TASK) {
                        msgIntType task_id_msg;
                        if (readSingleInt(i, &task_id_msg) < 0) {
                            // Eroare la citire, inchide conexiunea
                            close(i);
                            FD_CLR(i, &active_fd_set);
                            stats_decrement_clients();
                            continue;
                        }
                        int task_id = task_id_msg.msg;
                        int result = cancel_task(task_id);
                        if (result) {
                            writeSingleString(i, h, "Task cancelled successfully");
                        } else {
                            writeSingleString(i, h, "Task not found or already completed/cancelled");
                        }
                    }
                 
                    else if (operation == OPR_UPLOAD_FILE) {
                        char debug_msg[] = "[DEBUG] Received OPR_UPLOAD_FILE\n";
                        write(STDOUT_FILENO, debug_msg, strlen(debug_msg));
                        
                        int session_id = client_id;
                        if (!session_validate(session_id)) {
                            char err_msg[] = "Sesiune invalida\n";
                            write(STDOUT_FILENO, err_msg, strlen(err_msg));
                            writeSingleString(i, h, err_msg);
                            continue;
                        }
                        
                        write(STDOUT_FILENO, "[DEBUG] Session validated\n", 26);
                        
                        // 1. Citește numele fișierului
                        msgStringType filename;
                        if (readSingleString(i, &filename) < 0) {
                            write(STDOUT_FILENO, "[DEBUG] Failed to read filename\n", 32);
                            char err_msg[] = "Eroare la citirea numelui fisierului";
                            writeSingleString(i, h, err_msg);
                            continue;
                        }
                        
                        write(STDOUT_FILENO, "[DEBUG] Filename read successfully\n", 35);
                        
                        // 2. Citește dimensiunea fișierului
                        msgIntType size_msg;
                        if (readSingleInt(i, &size_msg) < 0) {
                            write(STDOUT_FILENO, "[DEBUG] Failed to read file size\n", 33);
                            free(filename.msg);
                            char err_msg[] = "Eroare la citirea dimensiunii";
                            writeSingleString(i, h, err_msg);
                            continue;
                        }
                        
                        char logbuf2[512];
                        snprintf(logbuf2, sizeof(logbuf2), "[DEBUG] File size: %d bytes\n", size_msg.msg);
                        write(STDOUT_FILENO, logbuf2, strlen(logbuf2));
                        size_t file_size = size_msg.msg;
                        
                        // 3. Citește parametrii GEO (ÎNAINTE de chunk-uri!)
                        msgStringType bbox_str;
                        char bbox[128] = "";
                        if (readSingleString(i, &bbox_str) >= 0 && strlen(bbox_str.msg) > 0) {
                            strncpy(bbox, bbox_str.msg, sizeof(bbox)-1);
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
                        
                        write(STDOUT_FILENO, "[DEBUG] GEO params read successfully\n", 36);
                        
                        // DEBUG: Afișează file_size, session_id și filename
                        snprintf(logbuf2, sizeof(logbuf2), "[DEBUG] file_size=%zu, session_id=%d, filename=%s\n", 
                                 file_size, session_id, filename.msg);
                        write(STDOUT_FILENO, logbuf2, strlen(logbuf2));
                        
                        // Sanitizează numele fișierului - păstrează doar ultima componentă după '/'
                        char *basename = strrchr(filename.msg, '/');
                        if (basename != NULL) {
                            basename++;
                        } else {
                            basename = filename.msg;
                        }
                        
                        // 4. Pregătește calea de scriere
                        char upload_path[512];
                        snprintf(upload_path, sizeof(upload_path), "processing/uploads/%d_%s",
                                 session_id, basename);
                        
                        char pathbuf[600];
                        snprintf(pathbuf, sizeof(pathbuf), "[DEBUG] upload_path=%s\n", upload_path);
                        write(STDOUT_FILENO, pathbuf, strlen(pathbuf));
                        
                        int out_fd = open(upload_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                        if (out_fd < 0) {
                            snprintf(logbuf2, sizeof(logbuf2), "[DEBUG] open failed: %s\n", strerror(errno));
                            write(STDOUT_FILENO, logbuf2, strlen(logbuf2));
                            free(filename.msg);
                            char err_msg[] = "Eroare la crearea fisierului";
                            writeSingleString(i, h, err_msg);
                            continue;
                        }
                        
                        snprintf(logbuf2, sizeof(logbuf2), "[DEBUG] out_fd=%d, about to enter while loop\n", out_fd);
                        write(STDOUT_FILENO, logbuf2, strlen(logbuf2));
                        
                        // 5. Primește chunk-uri de date brute
                        size_t received = 0;
                        char chunk[8192];
                        int chunk_result = 1;
                        
                        snprintf(logbuf2, sizeof(logbuf2), "[DEBUG] Entering while: received=%zu, file_size=%zu\n", received, file_size);
                        write(STDOUT_FILENO, logbuf2, strlen(logbuf2));
                        
                        while (received < file_size) {
                            size_t to_read = (file_size - received) < sizeof(chunk) ? (file_size - received) : sizeof(chunk);
                            
                            snprintf(logbuf2, sizeof(logbuf2), "[DEBUG] Calling recv, to_read=%zu\n", to_read);
                            write(STDOUT_FILENO, logbuf2, strlen(logbuf2));
                            
                            ssize_t n = recv(i, chunk, to_read, 0);
                            if (n <= 0) {
                                snprintf(logbuf2, sizeof(logbuf2), "[DEBUG] recv returned %zd, errno=%d (%s)\n", n, errno, strerror(errno));
                                write(STDOUT_FILENO, logbuf2, strlen(logbuf2));
                                chunk_result = -1;
                                break;
                            }
                            
                            snprintf(logbuf2, sizeof(logbuf2), "[DEBUG] recv got %zd bytes\n", n);
                            write(STDOUT_FILENO, logbuf2, strlen(logbuf2));
                            
                            ssize_t written = write(out_fd, chunk, n);
                            if (written != n) {
                                snprintf(logbuf2, sizeof(logbuf2), "[DEBUG] write failed: wrote %zd, expected %zd\n", written, n);
                                write(STDOUT_FILENO, logbuf2, strlen(logbuf2));
                                chunk_result = -1;
                                break;
                            }
                            received += n;
                            
                            snprintf(logbuf2, sizeof(logbuf2), "[DEBUG] received now %zu of %zu\n", received, file_size);
                            write(STDOUT_FILENO, logbuf2, strlen(logbuf2));
                        }
                        
                        close(out_fd);
                        
                        if (chunk_result < 0 || received != file_size) {
                            unlink(upload_path);
                            free(filename.msg);
                            char err_msg[] = "Eroare la transferul fisierului";
                            writeSingleString(i, h, err_msg);
                            continue;
                        }
                        
                        write(STDOUT_FILENO, "[DEBUG] File chunks received successfully\n", 40);
                        
                        // 6. Adaugă task-ul în coadă
                        int task_id = queue_add_task_file(basename, upload_path, session_id, i,
                                                          bbox, epsilon, show_segments,
                                                          dist_idx1, dist_idx2, request_id);
                        free(filename.msg);
                        
                        // 7. Trimite task_id înapoi
                        writeSingleInt(i, h, task_id);
                        
                        char hist_entry[256];
                        snprintf(hist_entry, sizeof(hist_entry), "Upload file: %s (%zu bytes) -> task %d",
                                 basename, file_size, task_id);
                        add_to_history(hist_entry);
                        
                        snprintf(logbuf2, sizeof(logbuf2), "[DEBUG] Task %d created\n", task_id);
                        write(STDOUT_FILENO, logbuf2, strlen(logbuf2));
                    }

                    else if (operation == OPR_DOWNLOAD_FILE) {
                        msgIntType task_id_msg;
                        if (readSingleInt(i, &task_id_msg) < 0) {
                            close(i);
                            FD_CLR(i, &active_fd_set);
                            stats_decrement_clients();
                            continue;
                        }
                        int task_id = task_id_msg.msg;
                        
                        // Păstrează request_id-ul primit de la client
                        int original_request_id = request_id;
                        
                        // Găsește task-ul în lista de finalizate
                        pthread_mutex_lock(&completed_mutex);
                        queue_task_t *task = completed_head;
                        while (task) {
                            if (task->task_id == task_id && task->status == 2) {
                                break;
                            }
                            task = task->next;
                        }
                        pthread_mutex_unlock(&completed_mutex);
                        
                        if (!task || task->output_path[0] == '\0') {
                            // Creează un header nou cu același request_id
                            msgHeaderType resp_h = h;
                            resp_h.requestID = original_request_id;
                            char err_msg[] = "ERROR: File not found or task not completed";
                            writeSingleString(i, resp_h, err_msg);
                            continue;
                        }
                        
                        // Deschide fișierul pentru citire
                        int fd = open(task->output_path, O_RDONLY);
                        if (fd < 0) {
                            msgHeaderType resp_h = h;
                            resp_h.requestID = original_request_id;
                            char err_msg[] = "ERROR: Cannot open result file";
                            writeSingleString(i, resp_h, err_msg);
                            continue;
                        }
                        
                        // Obține dimensiunea fișierului
                        off_t file_size = lseek(fd, 0, SEEK_END);
                        lseek(fd, 0, SEEK_SET);
                        
                        // Trimite metadate: nume fișier și dimensiune (folosește același request_id)
                        msgHeaderType resp_h = h;
                        resp_h.requestID = original_request_id;
                        
                        char fname[256];
                        snprintf(fname, sizeof(fname), "task_%d_result.csv", task_id);
                        writeSingleString(i, resp_h, fname);
                        writeSingleInt(i, resp_h, (int)file_size);
                        
                        // Trimite conținutul fișierului în chunk-uri
                        char chunk[8192];
                        ssize_t bytes;
                        off_t sent = 0;
                        while ((bytes = read(fd, chunk, sizeof(chunk))) > 0) {
                            ssize_t n = send(i, chunk, bytes, 0);
                            if (n != bytes) {
                                char err_msg[] = "ERROR: Transfer failed";
                                writeSingleString(i, resp_h, err_msg);
                                break;
                            }
                            sent += n;
                        }
                        close(fd);
                        
                        char logbuf3[512];
                        snprintf(logbuf3, sizeof(logbuf3), "[DOWNLOAD] Sent file %s (%ld bytes) to session %d",
                                 fname, (long)file_size, client_id);
                        log_message(logbuf3);
                    }
                    else if (operation == OPR_BYE) {
                        // Tratarea cererii de deconectare
                        int session_id = client_id;
                        if (session_id > 0) {
                            session_invalidate(session_id);
                            session_sock_remove(session_id);  // <--- ADĂUGĂ AICI
                        }
                        char bye_msg[] = "Deconectare realizata";
                        writeSingleString(i, h, bye_msg);
                        close(i);
                        FD_CLR(i, &active_fd_set);
                        stats_decrement_clients();
                    }
                    else {
                        // Operatie necunoscuta sau neimplementata
                        char err_msg[] = "Comanda necunoscuta";
                        writeSingleString(i, h, err_msg);
                    }
                }
            }
        }
    }
    
    pthread_exit(NULL);
}