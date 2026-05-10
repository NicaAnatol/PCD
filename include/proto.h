#ifndef PROTO_H
#define PROTO_H

// Pentru operatii de intrare/iesire, de exemplu printf()
#include <stdio.h>

// Pentru alocare dinamica si functii utilitare, de exemplu malloc(), free(), exit()
#include <stdlib.h>

// Pentru prelucrare de siruri de caractere, de exemplu strlen(), strcpy(), strcmp()
#include <string.h>

// Pentru apeluri de sistem precum read(), write(), close(), fork()
#include <unistd.h>

// Pentru coduri de eroare, folosind errno
#include <errno.h>

// Pentru thread-uri, de exemplu pthread_create(), pthread_mutex_lock()
#include <pthread.h>

// Pentru lucrul cu timpul, de exemplu time(), time_t
#include <time.h>

// Pentru functii matematice, de exemplu sin(), cos(), sqrt()
#include <math.h>

// Pentru tratarea semnalelor, de exemplu signal(), kill()
#include <signal.h>

// Pentru tipuri de baza folosite la socket-uri si procese
#include <sys/types.h>

// Pentru functii de socket, de exemplu socket(), bind(), accept(), connect()
#include <sys/socket.h>

// Pentru socket-uri Unix, folosind struct sockaddr_un
#include <sys/un.h>

// Pentru select(), folosit la multiplexarea descriptorilor
#include <sys/select.h>

// Pentru wait() si waitpid(), folosite la procese copil
#include <sys/wait.h>

// Pentru operatii pe fisiere/directoare, de exemplu mkdir(), stat()
#include <sys/stat.h>

// Pentru controlul fisierelor, de exemplu open(), fcntl()
#include <fcntl.h>

// Pentru conversii/adrese IP, de exemplu inet_pton(), inet_ntop()
#include <arpa/inet.h>

// Pentru structuri TCP/IP, cum ar fi sockaddr_in
#include <netinet/in.h>

// Pentru rezolvarea numelor de host, de exemplu gethostbyname(), getaddrinfo()
#include <netdb.h>

// Pentru operatii geometrice/geografice din biblioteca externa GEOS
#include <geos_c.h>

// Coduri de operatii (comenzi) intre client si server
#define OPR_UPLOAD_GEO   1   // upload fisier geografic
#define OPR_GET_DISTANCE 2   // calcul distanta
#define OPR_GET_BBOX     3   // obtinere bounding box
#define OPR_SIMPLIFY     4   // simplificare traseu
#define OPR_GET_STATUS   5   // status server
#define OPR_BYE          6   // inchidere conexiune

#define OPR_LOGIN       10   // autentificare utilizator
#define OPR_REGISTER    11   // inregistrare utilizator

// New opcodes for task management
#define OPR_CHECK_TASK    20
#define OPR_GET_RESULT    21
#define OPR_BLOCK_IP      30
#define OPR_UNBLOCK_IP    31
#define OPR_CANCEL_TASK   32
#define OPR_UPLOAD_FILE   50
#define OPR_DOWNLOAD_FILE 51
typedef struct task_result {
    char total_distance[64];
    char point_count[32];
    char segment_count[32];
    char segment_distances[1000][64]; // max 1000 segments
    int segment_cnt;
    char direct_distance[64];
    char route_distance[64];
    char has_request[8];
    char show_segments[8];
    char output_path[512];
    int ready;  // 1 if result is available
} task_result_t;

// Header comun pentru toate mesajele trimise prin socket
// MODIFIED: Adăugat câmpul requestID pentru corelare cerere-răspuns
typedef struct msgHeaderType {
    int msgSize;    // dimensiunea mesajului
    int clientID;   // ID-ul clientului
    int opID;       // codul operatiei
    int requestID;  // ID-ul cererii (pentru corelare)
} msgHeaderType;

// Mesaj simplu ce contine un int
typedef struct msgIntType {
    int msg;
} msgIntType;

// Mesaj ce contine un string
typedef struct msgStringType {
    char *msg;
} msgStringType;

// Mesaj complet cu header + int
typedef struct singleIntMsg {
    msgHeaderType header;
    msgIntType i;
} singleIntMsgType;

// Structura pentru un punct geografic
typedef struct pointMsgType {
    double lat; // latitudine
    double lon; // longitudine
} pointMsgType;

// Statistici rezultate din procesare geografica
typedef struct geoStatsMsgType {
    double total_distance;     // distanta totala calculata
    int point_count;           // numar puncte initiale
    int filtered_count;        // puncte dupa filtrare
    long processing_time_ms;   // timp procesare in milisecunde
} geoStatsMsgType;

// Statistici generale ale serverului
typedef struct server_stats {
    int active_clients;            // clienti conectati
    int active_processes;          // procese active
    int total_processed_points;    // total puncte procesate
    double total_processed_distance;// distanta totala procesata
    char last_upload[256];         // ultimul fisier incarcat
} server_stats_t;

// Functii pentru citire/scriere mesaje pe socket
msgHeaderType peekMsgHeader(int sock);
int readSingleInt(int sock, msgIntType *m);
int writeSingleInt(int sock, msgHeaderType h, int i);
int readSingleString(int sock, msgStringType *str);
int writeSingleString(int sock, msgHeaderType h, char *str);

// Functii pentru calcule geografice
double haversine_distance(pointMsgType p1, pointMsgType p2); // distanta intre doua puncte
double calculate_distance(pointMsgType *points, int count); // distanta totala traseu
double perpendicular_distance(pointMsgType p, pointMsgType a, pointMsgType b); // distanta perpendiculara

// Algoritm de simplificare traseu (Douglas-Peucker)
int douglas_peucker(pointMsgType *points, int count, double epsilon, pointMsgType **simplified);

// Procesare folosind procese copil (fork)
int process_with_children(const char *filename, int num_children, geoStatsMsgType *result);

// Thread-uri principale pentru server
void *unix_main(void *args); // server pe socket Unix
void *inet_main(void *args); // server pe TCP/IP

// Functii pentru statistici server
void stats_increment_clients(void);
void stats_decrement_clients(void);
void stats_add_processed(int points, double distance, const char *filename);
void stats_increment_processes(void);
void stats_decrement_processes(void);
void get_stats(server_stats_t *stats);
void *completed_task_cleanup(void *arg);

// Functii parsare fisiere
int parse_csv(const char *filename, pointMsgType **points);
int parse_gpx(const char *filename, pointMsgType **points);
int parse_geojson(const char *filename, pointMsgType **points);

// Coada de task-uri pentru procesare
int queue_add_task(const char *filename, int client_id);
int queue_add_task_file(const char *filename, const char *upload_path, int client_id, int sock_fd,
                        const char *bbox, double epsilon, int show_segments,
                        int dist_idx1, int dist_idx2, int request_id);
void *queue_processor(void *arg);

// Functii pipe notificare
int init_notify_pipe(void);
int get_notify_pipe_read_fd(void);
void notify_queue(void);
typedef struct queue_task {
    int task_id;                // identificator unic al task-ului
    int request_id;             // ID-ul cererii (pentru corelare)
    int cancel_flag;
    char filename[512];         // numele fisierului asociat cererii
    char upload_path[512];  
    char output_path[512];     
    char bbox[128];             // bounding box optional pentru filtrare
    double epsilon;             // prag pentru simplificare geometrica
    int show_segments;          // flag pentru afisarea segmentelor
    int dist_idx1;              // primul index pentru cererea de distanta
    int dist_idx2;              // al doilea index pentru cererea de distanta
    int client_id;              // ID-ul clientului care a trimis cererea
    int sock_fd;                // socket-ul pe care se trimite raspunsul
    int status;                 // starea task-ului: 0=pending,1=processing,2=done,3=cancelled
    time_t start_time;          // momentul inceperii task-ului
    time_t end_time;            // momentul finalizarii task-ului
    pointMsgType *points;       // vectorul de puncte primit de la client
    int point_count;            // numarul de puncte din cerere
    task_result_t result;       // store computed results
    struct queue_task *next;    // legatura spre urmatorul task din coada
} queue_task_t;
extern queue_task_t *completed_head;
extern pthread_mutex_t completed_mutex;
// Domain blacklist functions
void domain_blacklist_add(const char *domain);
void domain_blacklist_remove(const char *domain);
int domain_blacklist_check(const char *domain);
int force_disconnect_client(int session_id);

// Structura pentru sesiune client (autentificare)
typedef struct client_session {
    int session_id;           // ID sesiune
    char username[64];        // username utilizator
    int authenticated;        // flag autentificare
    time_t login_time;        // moment login
    time_t last_activity;     // ultima activitate
    struct client_session *next; // pointer pentru lista de sesiuni
} client_session_t;

// Functii pentru management sesiuni
int session_create(const char *username);
int session_validate(int session_id);
void session_invalidate(int session_id);
void session_update_activity(int session_id);
void session_sock_add(int session_id, int sock_fd);
void session_sock_remove(int session_id);
int session_sock_get_fd(int session_id);
int force_disconnect_client(int session_id);
// Structura pentru rezultat procesare geografica
typedef struct geo_result {
    double total_distance;         // distanta totala
    int point_count;               // numar puncte
    double segment_distances[1000];// distante pe segmente
    int segment_count;             // numar segmente
    double direct_distance;        // distanta directa intre capete
    double route_distance;         // distanta traseu complet
    int has_distance_request;      // flag daca exista cerere de distanta
} geo_result_t;

#endif 