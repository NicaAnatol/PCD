#ifndef PROTO_H
#define PROTO_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <geos_c.h>

#define OPR_UPLOAD_GEO   1
#define OPR_GET_DISTANCE 2
#define OPR_GET_BBOX     3
#define OPR_SIMPLIFY     4
#define OPR_GET_STATUS   5
#define OPR_BYE          6

#define OPR_LOGIN       10
#define OPR_REGISTER    11

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

typedef struct server_stats {
    int active_clients;
    int active_processes;
    int total_processed_points;
    double total_processed_distance;
    char last_upload[256];
} server_stats_t;

msgHeaderType peekMsgHeader(int sock);
int readSingleInt(int sock, msgIntType *m);
int writeSingleInt(int sock, msgHeaderType h, int i);
int readSingleString(int sock, msgStringType *str);
int writeSingleString(int sock, msgHeaderType h, char *str);

double haversine_distance(pointMsgType p1, pointMsgType p2);
double calculate_distance(pointMsgType *points, int count);
double perpendicular_distance(pointMsgType p, pointMsgType a, pointMsgType b);
int douglas_peucker(pointMsgType *points, int count, double epsilon, pointMsgType **simplified);
int process_with_children(const char *filename, int num_children, geoStatsMsgType *result);

void *unix_main(void *args);
void *inet_main(void *args);

void stats_increment_clients(void);
void stats_decrement_clients(void);
void stats_add_processed(int points, double distance, const char *filename);
void stats_increment_processes(void);
void stats_decrement_processes(void);
void get_stats(server_stats_t *stats);

int queue_add_task(const char *filename, int client_id);
void *queue_processor(void *arg);

#endif 

typedef struct client_session {
    int session_id;
    char username[64];
    int authenticated;
    time_t login_time;
    time_t last_activity;
    struct client_session *next;
} client_session_t;

int session_create(const char *username);
int session_validate(int session_id);
void session_invalidate(int session_id);
void session_update_activity(int session_id);

typedef struct geo_result {
    double total_distance;
    int point_count;
    double segment_distances[1000];  
    int segment_count;
    double direct_distance;          
    double route_distance;          
    int has_distance_request;        
} geo_result_t;
