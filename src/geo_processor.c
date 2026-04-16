#include "../include/logging.h"
#include "../include/proto.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

double haversine_distance(pointMsgType p1, pointMsgType p2) {
    double R = 6371.0;
    double lat1 = p1.lat * M_PI / 180.0;
    double lat2 = p2.lat * M_PI / 180.0;
    double dlat = (p2.lat - p1.lat) * M_PI / 180.0;
    double dlon = (p2.lon - p1.lon) * M_PI / 180.0;
    
    double a = sin(dlat/2) * sin(dlat/2) +
               cos(lat1) * cos(lat2) *
               sin(dlon/2) * sin(dlon/2);
    double c = 2 * atan2(sqrt(a), sqrt(1-a));
    
    return R * c;
}

double calculate_distance(pointMsgType *points, int count) {
    if (count < 2) return 0.0;
    double total = 0.0;
    for (int i = 0; i < count - 1; i++) {
        total += haversine_distance(points[i], points[i+1]);
    }
    return total;
}

double perpendicular_distance(pointMsgType p, pointMsgType a, pointMsgType b) {
    double lat1 = a.lat, lon1 = a.lon;
    double lat2 = b.lat, lon2 = b.lon;
    double latp = p.lat, lonp = p.lon;
    
    double dx = lat2 - lat1;
    double dy = lon2 - lon1;
    
    if (dx == 0 && dy == 0) {
        return haversine_distance(p, a);
    }
    
    double t = ((latp - lat1) * dx + (lonp - lon1) * dy) / (dx * dx + dy * dy);
    
    if (t < 0) return haversine_distance(p, a);
    if (t > 1) return haversine_distance(p, b);
    
    pointMsgType proj;
    proj.lat = lat1 + t * dx;
    proj.lon = lon1 + t * dy;
    
    return haversine_distance(p, proj);
}

static void douglas_peucker_recursive(pointMsgType *points, int start, int end,
                                      double epsilon, int *keep, int *keep_count) {
    if (end <= start + 1) return;
    
    double dmax = 0;
    int index = start;
    
    for (int i = start + 1; i < end; i++) {
        double d = perpendicular_distance(points[i], points[start], points[end]);
        if (d > dmax) {
            dmax = d;
            index = i;
        }
    }
    
    if (dmax > epsilon) {
        keep[index] = 1;
        (*keep_count)++;
        douglas_peucker_recursive(points, start, index, epsilon, keep, keep_count);
        douglas_peucker_recursive(points, index, end, epsilon, keep, keep_count);
    }
}

int douglas_peucker(pointMsgType *points, int count, double epsilon, pointMsgType **simplified) {
    if (count < 3) {
        *simplified = malloc(sizeof(pointMsgType) * count);
        if (*simplified) memcpy(*simplified, points, sizeof(pointMsgType) * count);
        return count;
    }
    
    int *keep = calloc(count, sizeof(int));
    if (!keep) return -1;
    
    keep[0] = 1;
    keep[count - 1] = 1;
    int keep_count = 2;
    
    douglas_peucker_recursive(points, 0, count - 1, epsilon, keep, &keep_count);
    
    *simplified = malloc(sizeof(pointMsgType) * keep_count);
    if (!*simplified) {
        free(keep);
        return -1;
    }
    
    int idx = 0;
    for (int i = 0; i < count; i++) {
        if (keep[i]) (*simplified)[idx++] = points[i];
    }
    
    free(keep);
    return keep_count;
}

int process_with_children(const char *filename, int num_children, geoStatsMsgType *result) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        log_message("[ERROR] Cannot open file");
        return -1;
    }
    
    pointMsgType points[10000];
    int point_count = 0;
    char buffer[4096];
    ssize_t bytes;
    
    while ((bytes = read(fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes] = '\0';
        char *line = buffer;
        char *next;
        while ((next = strchr(line, '\n')) != NULL) {
            *next = '\0';
            if (sscanf(line, "%lf,%lf", &points[point_count].lat, &points[point_count].lon) == 2) {
                point_count++;
            }
            line = next + 1;
        }
    }
    close(fd);
    
    if (point_count == 0) {
        log_message("[ERROR] No points in file");
        return -1;
    }
    
    int points_per_child = point_count / num_children;
    if (points_per_child < 1) points_per_child = 1;
    
    int pipes[num_children][2];
    pid_t pids[num_children];
    
    for (int i = 0; i < num_children; i++) {
        if (pipe(pipes[i]) < 0) return -1;
        
        pids[i] = fork();
        
        if (pids[i] == 0) {
            close(pipes[i][0]);
            int start = i * points_per_child;
            int end = (i == num_children - 1) ? point_count : start + points_per_child;
            double dist = calculate_distance(points + start, end - start);
            write(pipes[i][1], &dist, sizeof(double));
            close(pipes[i][1]);
            _exit(0);
        } else if (pids[i] < 0) {
            return -1;
        }
        close(pipes[i][1]);
    }
    
    double total_distance = 0.0;
    for (int i = 0; i < num_children; i++) {
        int status;
        waitpid(pids[i], &status, 0);
        double dist;
        read(pipes[i][0], &dist, sizeof(double));
        total_distance += dist;
        close(pipes[i][0]);
    }
    
    result->total_distance = total_distance;
    result->point_count = point_count;
    result->filtered_count = point_count;
    result->processing_time_ms = 0;
    
    char logbuf[256];
    snprintf(logbuf, sizeof(logbuf), "[GEO] Processed %d points with %d children, distance=%.2f km",
             point_count, num_children, total_distance);
    log_message(logbuf);
    
    return 0;
}
