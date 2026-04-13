#ifndef GEO_PROTO_H
#define GEO_PROTO_H

#include <stddef.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <math.h>
#include <libconfig.h>

/* Operatiuni pentru procesare geo-spatiala */
#define OPR_UPLOAD_GEO   1   /* Upload fisier cu coordonate */
#define OPR_GET_DISTANCE 2   /* Calculeaza distanta totala */
#define OPR_GET_BBOX     3   /* Filtrare puncte in bounding box */
#define OPR_SIMPLIFY     4   /* Simplificare traseu (Douglas-Peucker) */
#define OPR_GET_STATUS   5   /* Status procesare */
#define OPR_BYE          6   /* Inchidere conexiune */

/* Structuri pentru mesaje - la fel ca in proto.h */
typedef struct msgHeader {
    int msgSize;        /* dimensiunea mesajului */
    int clientID;       /* ID-ul clientului */
    int opID;           /* operatia ceruta */
} msgHeaderType;

typedef struct pointMsg {
    double lat;         /* latitudine */
    double lon;         /* longitudine */
} pointMsgType;

typedef struct geoStatsMsg {
    double total_distance;   /* distanta totala in km */
    int point_count;         /* numar puncte */
    int filtered_count;      /* puncte dupa filtrare */
    long processing_time_ms; /* timp procesare */
} geoStatsMsgType;

/* Prototipuri functii */
msgHeaderType peekMsgHeader(int sock);
int readSingleInt(int sock, msgIntType *m);
int writeSingleInt(int sock, msgHeaderType h, int i);
int readSingleString(int sock, msgStringType *str);
int writeSingleString(int sock, msgHeaderType h, char *str);
int readGeoPoints(int sock, pointMsgType **points, int *count);
int writeGeoStats(int sock, msgHeaderType h, geoStatsMsgType *stats);

/* Functii geo-spatiale */
double calculate_distance(pointMsgType *points, int count);
int filter_by_bbox(pointMsgType *points, int count, double min_lat, double max_lat,
                   double min_lon, double max_lon, pointMsgType **filtered);
int douglas_peucker(pointMsgType *points, int count, double epsilon,
                    pointMsgType **simplified);
void process_geo_file(const char *filename, int sock, int client_id);

/* Fork processing */
int process_with_children(const char *filename, int num_children,
                          geoStatsMsgType *result);

/* Logging low-level */
void log_message(const char *msg);
void log_int(const char *prefix, int value);
void log_double(const char *prefix, double value);

/* Configurare */
int load_config(const char *config_file);

#endif /* GEO_PROTO_H */