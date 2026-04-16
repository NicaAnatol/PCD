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

#define OPR_UPLOAD_GEO   1
#define OPR_GET_DISTANCE 2
#define OPR_GET_BBOX     3
#define OPR_SIMPLIFY     4
#define OPR_GET_STATUS   5
#define OPR_BYE          6

typedef struct msgHeaderType {
    int msgSize;
    int clientID;
    int opID;
} msgHeaderType;

typedef struct msgIntType {
    int msg;
} msgIntType;

typedef struct msgStringType {
    char *msg;
} msgStringType;

typedef struct singleIntMsg {
    msgHeaderType header;
    msgIntType i;
} singleIntMsgType;

typedef struct pointMsgType {
    double lat;
    double lon;
} pointMsgType;

typedef struct geoStatsMsgType {
    double total_distance;
    int point_count;
    int filtered_count;
    long processing_time_ms;
} geoStatsMsgType;

msgHeaderType peekMsgHeader(int sock);
int readSingleInt(int sock, msgIntType *m);
int writeSingleInt(int sock, msgHeaderType h, int i);
int readSingleString(int sock, msgStringType *str);
int writeSingleString(int sock, msgHeaderType h, char *str);
int readGeoPoints(int sock, pointMsgType **points, int *count);
int writeGeoStats(int sock, msgHeaderType h, geoStatsMsgType *stats);

double calculate_distance(pointMsgType *points, int count);
double haversine_distance(pointMsgType p1, pointMsgType p2);
double perpendicular_distance(pointMsgType p, pointMsgType a, pointMsgType b);
int filter_by_bbox(pointMsgType *points, int count, double min_lat, double max_lat,
                   double min_lon, double max_lon, pointMsgType **filtered);
int douglas_peucker(pointMsgType *points, int count, double epsilon,
                    pointMsgType **simplified);
void process_geo_file(const char *filename, int sock, int client_id);
int process_with_children(const char *filename, int num_children,
                          geoStatsMsgType *result);

void log_init(const char *filename);
void log_message(const char *msg);
void log_int(const char *prefix, int value);
void log_double(const char *prefix, double value);

int load_config(const char *config_file);

#endif 
