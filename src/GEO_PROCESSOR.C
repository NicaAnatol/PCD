#include "../include/geo_proto.h"

/* Formula Haversine pentru distanta intre doua puncte (km) */
double haversine_distance(pointMsgType p1, pointMsgType p2) {
    double R = 6371.0; /* Raza Pamantului in km */
    
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

/* Calculeaza distanta totala a unui traseu */
double calculate_distance(pointMsgType *points, int count) {
    if (count < 2) return 0.0;
    
    double total = 0.0;
    for (int i = 0; i < count - 1; i++) {
        total += haversine_distance(points[i], points[i+1]);
    }
    return total;
}

/* Filtrare puncte in bounding box */
int filter_by_bbox(pointMsgType *points, int count, double min_lat, double max_lat,
                   double min_lon, double max_lon, pointMsgType **filtered) {
    int filtered_count = 0;
    *filtered = malloc(sizeof(pointMsgType) * count);
    if (*filtered == NULL) return -1;
    
    for (int i = 0; i < count; i++) {
        if (points[i].lat >= min_lat && points[i].lat <= max_lat &&
            points[i].lon >= min_lon && points[i].lon <= max_lon) {
            (*filtered)[filtered_count++] = points[i];
        }
    }
    
    return filtered_count;
}

/* Douglas-Peucker - simplificare traseu */
static void douglas_peucker_recursive(pointMsgType *points, int start, int end,
                                      double epsilon, int *keep, int *keep_count) {
    if (end <= start + 1) return;
    
    /* Găsește punctul cu distanta maxima */
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
        /* Păstrează punctul și procesează recursiv */
        keep[index] = 1;
        (*keep_count)++;
        douglas_peucker_recursive(points, start, index, epsilon, keep, keep_count);
        douglas_peucker_recursive(points, index, end, epsilon, keep, keep_count);
    }
}

int douglas_peucker(pointMsgType *points, int count, double epsilon,
                    pointMsgType **simplified) {
    if (count < 3) {
        *simplified = malloc(sizeof(pointMsgType) * count);
        if (*simplified == NULL) return -1;
        memcpy(*simplified, points, sizeof(pointMsgType) * count);
        return count;
    }
    
    int *keep = calloc(count, sizeof(int));
    if (keep == NULL) return -1;
    
    keep[0] = 1;
    keep[count - 1] = 1;
    int keep_count = 2;
    
    douglas_peucker_recursive(points, 0, count - 1, epsilon, keep, &keep_count);
    
    *simplified = malloc(sizeof(pointMsgType) * keep_count);
    if (*simplified == NULL) {
        free(keep);
        return -1;
    }
    
    int idx = 0;
    for (int i = 0; i < count; i++) {
        if (keep[i]) {
            (*simplified)[idx++] = points[i];
        }
    }
    
    free(keep);
    return keep_count;
}

/* Procesare fisier cu fork() si pipe() */
int process_with_children(const char *filename, int num_children,
                          geoStatsMsgType *result) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        log_message("[ERROR] Cannot open file");
        return -1;
    }
    
    /* Citire puncte din fisier */
    pointMsgType points[10000];
    int point_count = 0;
    char buffer[512];
    ssize_t bytes;
    
    while ((bytes = read(fd, buffer, sizeof(buffer))) > 0) {
        /* Parsare simpla: "lat,lon\n" */
        char *line = buffer;
        char *next;
        while ((next = strchr(line, '\n')) != NULL) {
            *next = '\0';
            sscanf(line, "%lf,%lf", &points[point_count].lat, &points[point_count].lon);
            point_count++;
            line = next + 1;
        }
    }
    close(fd);
    
    if (point_count == 0) {
        log_message("[ERROR] No points in file");
        return -1;
    }
    
    /* Fork procese copii */
    int points_per_child = point_count / num_children;
    int pipes[num_children][2];
    pid_t pids[num_children];
    double distances[num_children];
    
    for (int i = 0; i < num_children; i++) {
        if (pipe(pipes[i]) < 0) {
            char err[] = "[ERROR] pipe failed\n";
            write(STDERR_FILENO, err, sizeof(err)-1);
            return -1;
        }
        
        pids[i] = fork();
        
        if (pids[i] == 0) {
            /* Proces copil */
            close(pipes[i][0]); /* Inchide capatul de citire */
            
            int start = i * points_per_child;
            int end = (i == num_children - 1) ? point_count : start + points_per_child;
            
            double dist = calculate_distance(points + start, end - start);
            
            write(pipes[i][1], &dist, sizeof(double));
            close(pipes[i][1]);
            _exit(0);
        }
        else if (pids[i] < 0) {
            char err[] = "[ERROR] fork failed\n";
            write(STDERR_FILENO, err, sizeof(err)-1);
            return -1;
        }
        
        /* Proces parinte */
        close(pipes[i][1]);
    }
    
    /* Așteaptă copiii și colectează rezultatele */
    double total_distance = 0.0;
    for (int i = 0; i < num_children; i++) {
        int status;
        waitpid(pids[i], &status, 0);
        
        read(pipes[i][0], &distances[i], sizeof(double));
        total_distance += distances[i];
        close(pipes[i][0]);
    }
    
    result->total_distance = total_distance;
    result->point_count = point_count;
    result->filtered_count = point_count;
    result->processing_time_ms = 0;
    
    char logbuf[256];
    int len = snprintf(logbuf, sizeof(logbuf),
                       "[GEO] Processed %d points with %d children, distance=%.2f km\n",
                       point_count, num_children, total_distance);
    write(STDERR_FILENO, logbuf, len);
    
    return 0;
}