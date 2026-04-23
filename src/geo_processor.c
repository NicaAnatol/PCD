#include "../include/logging.h"   // Pentru functiile de logare folosite la raportarea starilor si erorilor
#include "../include/proto.h"     // Pentru structurile pointMsgType, geoStatsMsgType si prototipurile asociate
#include <geos_c.h>               // Biblioteca externa GEOS folosita pentru operatii geometrice
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Context global GEOS, necesar pentru apelurile reentrante din biblioteca
static GEOSContextHandle_t geos_ctx = NULL;

// Initializeaza contextul GEOS care va fi folosit de functiile geometrice.
// Daca initializarea esueaza, se scrie un mesaj in log.
void geos_init(void) {
    geos_ctx = GEOS_init_r();
    if (!geos_ctx) {
        log_message("[GEOS] Failed to initialize GEOS");
    } else {
        log_message("[GEOS] GEOS initialized successfully");
    }
}

// Elibereaza contextul GEOS la finalul executiei pentru a evita scurgerile de resurse.
void geos_cleanup(void) {
    if (geos_ctx) {
        GEOS_finish_r(geos_ctx);
        geos_ctx = NULL;
    }
}

// Construieste un obiect geometric de tip LineString pornind de la un vector de puncte.
// Functia este utila pentru a putea aplica direct operatii GEOS pe traseu.
static GEOSGeometry* points_to_linestring(pointMsgType *points, int count) {
    if (!geos_ctx || count < 2) return NULL;
    
    // Creeaza o secventa de coordonate cu cate doua valori pentru fiecare punct: X si Y
    GEOSCoordSequence *seq = GEOSCoordSeq_create_r(geos_ctx, count, 2);
    if (!seq) return NULL;
    
    // In GEOS, coordonatele sunt puse ca X = longitudine si Y = latitudine
    for (int i = 0; i < count; i++) {
        GEOSCoordSeq_setX_r(geos_ctx, seq, i, points[i].lon);
        GEOSCoordSeq_setY_r(geos_ctx, seq, i, points[i].lat);
    }
    
    return GEOSGeom_createLineString_r(geos_ctx, seq);
}

// Calculeaza distanta dintre doua puncte geografice.
// Daca GEOS este disponibil, incearca folosirea lui; altfel aplica formula Haversine.
double haversine_distance(pointMsgType p1, pointMsgType p2) {
    if (geos_ctx) {
        // Creeaza o secventa de coordonate pentru primul punct
        GEOSCoordSequence *seq = GEOSCoordSeq_create_r(geos_ctx, 2, 2);
        if (seq) {
            GEOSCoordSeq_setX_r(geos_ctx, seq, 0, p1.lon);
            GEOSCoordSeq_setY_r(geos_ctx, seq, 0, p1.lat);
            GEOSCoordSeq_setX_r(geos_ctx, seq, 1, p2.lon);
            GEOSCoordSeq_setY_r(geos_ctx, seq, 1, p2.lat);
            
            // Construieste primul punct geometric
            GEOSGeometry *g1 = GEOSGeom_createPoint_r(geos_ctx, seq);
            
            // Construieste al doilea punct geometric
            GEOSCoordSequence *seq2 = GEOSCoordSeq_create_r(geos_ctx, 2, 2);
            GEOSCoordSeq_setX_r(geos_ctx, seq2, 0, p2.lon);
            GEOSCoordSeq_setY_r(geos_ctx, seq2, 0, p2.lat);
            GEOSGeometry *g2 = GEOSGeom_createPoint_r(geos_ctx, seq2);
            
            // Calculeaza distanta dintre cele doua geometrii
            double dist;
            GEOSDistance_r(geos_ctx, g1, g2, &dist);
            
            // Elibereaza obiectele GEOS create
            GEOSGeom_destroy_r(geos_ctx, g1);
            GEOSGeom_destroy_r(geos_ctx, g2);
            
            return dist;
        }
    }
    
    // Varianta de rezerva: formula Haversine pentru distanta pe suprafata Pamantului
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

// Calculeaza distanta totala a unui traseu format din mai multe puncte.
// Se incearca utilizarea GEOS pentru lungimea unui LineString, iar daca nu merge,
// se insumeaza distantele dintre punctele consecutive.
double calculate_distance(pointMsgType *points, int count) {
    if (count < 2) return 0.0;
    
    if (geos_ctx && count >= 2) {
        GEOSGeometry *line = points_to_linestring(points, count);
        if (line) {
            double length;
            GEOSLength_r(geos_ctx, line, &length);
            GEOSGeom_destroy_r(geos_ctx, line);
            return length;
        }
    }
    
    // Varianta clasica: suma distantelor dintre punctele consecutive
    double total = 0.0;
    for (int i = 0; i < count - 1; i++) {
        total += haversine_distance(points[i], points[i+1]);
    }
    return total;
}

// Calculeaza distanta perpendiculara de la un punct p la segmentul [a, b].
// Este folosita in algoritmul Douglas-Peucker pentru simplificarea traseelor.
double perpendicular_distance(pointMsgType p, pointMsgType a, pointMsgType b) {
    double lat1 = a.lat, lon1 = a.lon;
    double lat2 = b.lat, lon2 = b.lon;
    double latp = p.lat, lonp = p.lon;
    
    double dx = lat2 - lat1;
    double dy = lon2 - lon1;
    
    // Daca segmentul este degenerat, se considera distanta pana la punctul a
    if (dx == 0 && dy == 0) {
        return haversine_distance(p, a);
    }
    
    // Calculeaza proiectia punctului pe segment
    double t = ((latp - lat1) * dx + (lonp - lon1) * dy) / (dx * dx + dy * dy);
    
    // Daca proiectia iese din segment, se ia distanta fata de capatul cel mai apropiat
    if (t < 0) return haversine_distance(p, a);
    if (t > 1) return haversine_distance(p, b);
    
    pointMsgType proj;
    proj.lat = lat1 + t * dx;
    proj.lon = lon1 + t * dy;
    
    return haversine_distance(p, proj);
}

// Functie recursiva pentru algoritmul Douglas-Peucker.
// Marcheaza punctele care trebuie pastrate in traseul simplificat.
static void douglas_peucker_recursive(pointMsgType *points, int start, int end,
                                      double epsilon, int *keep, int *keep_count) {
    if (end <= start + 1) return;
    
    double dmax = 0;
    int index = start;
    
    // Cauta punctul cu distanta maxima fata de segmentul dintre capete
    for (int i = start + 1; i < end; i++) {
        double d = perpendicular_distance(points[i], points[start], points[end]);
        if (d > dmax) {
            dmax = d;
            index = i;
        }
    }
    
    // Daca distanta maxima depaseste pragul epsilon, punctul trebuie pastrat
    if (dmax > epsilon) {
        keep[index] = 1;
        (*keep_count)++;
        douglas_peucker_recursive(points, start, index, epsilon, keep, keep_count);
        douglas_peucker_recursive(points, index, end, epsilon, keep, keep_count);
    }
}

// Simplifica un traseu folosind algoritmul Douglas-Peucker.
// Daca GEOS este disponibil, incearca mai intai simplificarea prin biblioteca externa.
int douglas_peucker(pointMsgType *points, int count, double epsilon, pointMsgType **simplified) {
    if (count < 3) {
        // Pentru mai putin de 3 puncte, nu are sens simplificarea; se copiaza traseul original
        *simplified = malloc(sizeof(pointMsgType) * count);
        if (*simplified) memcpy(*simplified, points, sizeof(pointMsgType) * count);
        return count;
    }
    
    if (geos_ctx) {
        // Incearca simplificarea cu functia oferita de GEOS
        GEOSGeometry *line = points_to_linestring(points, count);
        if (line) {
            GEOSGeometry *simplified_geom = GEOSSimplify_r(geos_ctx, line, epsilon);
            if (simplified_geom) {
                int simplified_count = GEOSGetNumCoordinates_r(geos_ctx, simplified_geom);
                if (simplified_count > 0 && simplified_count < count) {
                    *simplified = malloc(sizeof(pointMsgType) * simplified_count);
                    if (*simplified) {
                        const GEOSCoordSequence *seq = GEOSGeom_getCoordSeq_r(geos_ctx, simplified_geom);
                        for (int i = 0; i < simplified_count; i++) {
                            double x, y;
                            GEOSCoordSeq_getX_r(geos_ctx, seq, i, &x);
                            GEOSCoordSeq_getY_r(geos_ctx, seq, i, &y);
                            (*simplified)[i].lat = y;
                            (*simplified)[i].lon = x;
                        }
                    }
                    GEOSGeom_destroy_r(geos_ctx, simplified_geom);
                    GEOSGeom_destroy_r(geos_ctx, line);
                    return simplified_count;
                }
                GEOSGeom_destroy_r(geos_ctx, simplified_geom);
            }
            GEOSGeom_destroy_r(geos_ctx, line);
        }
    }

    // Varianta manuala, folosind algoritmul recursiv implementat local
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
    
    // Construieste vectorul final doar din punctele marcate pentru pastrare
    int idx = 0;
    for (int i = 0; i < count; i++) {
        if (keep[i]) (*simplified)[idx++] = points[i];
    }
    
    free(keep);
    return keep_count;
}

// Proceseaza un fisier cu puncte geografice folosind mai multe procese copil.
// Fiecare copil calculeaza distanta pe o portiune din date, iar parintele agregheaza rezultatele.
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
    
    // Citeste fisierul si extrage punctele de forma lat,lon
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
    
    // Daca nu s-a extras niciun punct valid, procesarea nu poate continua
    if (point_count == 0) {
        log_message("[ERROR] No points in file");
        return -1;
    }
    
    // Impartirea aproximativa a punctelor intre procesele copil
    int points_per_child = point_count / num_children;
    if (points_per_child < 1) points_per_child = 1;
    
    int pipes[num_children][2];
    pid_t pids[num_children];
    
    // Creeaza procese copil si canale pipe pentru comunicarea rezultatelor catre parinte
    for (int i = 0; i < num_children; i++) {
        if (pipe(pipes[i]) < 0) return -1;
        
        pids[i] = fork();
        
        if (pids[i] == 0) {
            // Cod executat in procesul copil
            close(pipes[i][0]);
            int start = i * points_per_child;
            int end = (i == num_children - 1) ? point_count : start + points_per_child;
            double dist = calculate_distance(points + start, end - start);
            write(pipes[i][1], &dist, sizeof(double));
            close(pipes[i][1]);
            _exit(0);
        } else if (pids[i] < 0) {
            // Eroare la creare proces copil
            return -1;
        }
        
        // Procesul parinte inchide capatul de scriere al pipe-ului
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
    
    // Completeaza structura de rezultat cu valorile obtinute
    result->total_distance = total_distance;
    result->point_count = point_count;
    result->filtered_count = point_count;
    result->processing_time_ms = 0;
    
    // Scrie in log un rezumat al procesarii
    char logbuf[256];
    snprintf(logbuf, sizeof(logbuf), "[GEO] Processed %d points with %d children, distance=%.2f km",
             point_count, num_children, total_distance);
    log_message(logbuf);
    
    return 0;
}