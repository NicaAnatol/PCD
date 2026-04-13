#include "../include/geo_proto.h"
#include "../include/proto.h"

/* Implementation of msgHeaderType peekMsgHeader */
msgHeaderType peekMsgHeader(int sock) {
    ssize_t nb;
    msgHeaderType h;
    h.msgSize = htonl(sizeof(h));
    nb = recv(sock, &h, sizeof(h), MSG_PEEK | MSG_WAITALL);
    
    h.msgSize = ntohl(h.msgSize);
    h.clientID = ntohl(h.clientID);
    h.opID = ntohl(h.opID);
    
    if (nb == -1 || nb == 0) {
        h.opID = h.clientID = -1;
    }
    
    char logbuf[256];
    int len = snprintf(logbuf, sizeof(logbuf), 
                       "[PROTO] peekMsgHeader: size=%d client=%d op=%d\n",
                       h.msgSize, h.clientID, h.opID);
    write(STDERR_FILENO, logbuf, len);
    
    return h;
}

int readSingleInt(int sock, msgIntType *m) {
    ssize_t nb;
    singleIntMsgType s;
    nb = recv(sock, &s, sizeof(s), MSG_WAITALL);
    
    if (nb <= 0) {
        m->msg = -1;
        return -1;
    }
    m->msg = ntohl(s.i.msg);
    
    char logbuf[128];
    int len = snprintf(logbuf, sizeof(logbuf), 
                       "[PROTO] readSingleInt: %d\n", m->msg);
    write(STDERR_FILENO, logbuf, len);
    
    return nb;
}

int writeSingleInt(int sock, msgHeaderType h, int i) {
    singleIntMsgType s;
    s.header.clientID = htonl(h.clientID);
    s.header.opID = htonl(h.opID);
    s.i.msg = htonl(i);
    s.header.msgSize = htonl(sizeof(s));
    
    ssize_t nb = send(sock, &s, sizeof(s), 0);
    
    if (nb == -1 || nb == 0) {
        return -1;
    }
    
    char logbuf[128];
    int len = snprintf(logbuf, sizeof(logbuf), 
                       "[PROTO] writeSingleInt: %d\n", i);
    write(STDERR_FILENO, logbuf, len);
    
    return nb;
}

int readSingleString(int sock, msgStringType *str) {
    msgIntType m;
    ssize_t nb = readSingleInt(sock, &m);
    if (nb < 0) return -1;
    
    int strSize = m.msg;
    if (strSize <= 0 || strSize > 65536) {
        char err[] = "[PROTO] Invalid string size\n";
        write(STDERR_FILENO, err, sizeof(err)-1);
        return -1;
    }
    
    str->msg = malloc(strSize + 1);
    if (str->msg == NULL) return -1;
    
    nb = recv(sock, str->msg, strSize, MSG_WAITALL);
    if (nb <= 0) {
        free(str->msg);
        str->msg = NULL;
        return -1;
    }
    str->msg[nb] = '\0';
    
    char logbuf[256];
    int len = snprintf(logbuf, sizeof(logbuf), 
                       "[PROTO] readSingleString: %s\n", str->msg);
    write(STDERR_FILENO, logbuf, len);
    
    return nb;
}

int writeSingleString(int sock, msgHeaderType h, char *str) {
    int strSize = strlen(str);
    ssize_t nb = writeSingleInt(sock, h, strSize);
    if (nb < 0) return -1;
    
    nb = send(sock, str, strSize, 0);
    if (nb == -1 || nb == 0) return -1;
    
    char logbuf[256];
    int len = snprintf(logbuf, sizeof(logbuf), 
                       "[PROTO] writeSingleString: %s\n", str);
    write(STDERR_FILENO, logbuf, len);
    
    return nb;
}

int readGeoPoints(int sock, pointMsgType **points, int *count) {
    msgIntType m;
    ssize_t nb = readSingleInt(sock, &m);
    if (nb < 0) return -1;
    
    *count = m.msg;
    if (*count <= 0 || *count > 100000) {
        char err[] = "[PROTO] Invalid point count\n";
        write(STDERR_FILENO, err, sizeof(err)-1);
        return -1;
    }
    
    *points = malloc(sizeof(pointMsgType) * (*count));
    if (*points == NULL) return -1;
    
    nb = recv(sock, *points, sizeof(pointMsgType) * (*count), MSG_WAITALL);
    if (nb <= 0) {
        free(*points);
        *points = NULL;
        return -1;
    }
    
    char logbuf[128];
    int len = snprintf(logbuf, sizeof(logbuf), 
                       "[PROTO] readGeoPoints: %d points\n", *count);
    write(STDERR_FILENO, logbuf, len);
    
    return nb;
}

int writeGeoStats(int sock, msgHeaderType h, geoStatsMsgType *stats) {
    ssize_t nb = send(sock, stats, sizeof(geoStatsMsgType), 0);
    if (nb == -1 || nb == 0) return -1;
    
    char logbuf[256];
    int len = snprintf(logbuf, sizeof(logbuf), 
                       "[PROTO] writeGeoStats: dist=%.2f km points=%d filtered=%d time=%ld\n",
                       stats->total_distance, stats->point_count,
                       stats->filtered_count, stats->processing_time_ms);
    write(STDERR_FILENO, logbuf, len);
    
    return nb;
}