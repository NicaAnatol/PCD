#include "../include/config.h"    // Declaratii pentru structura de configurare si functiile asociate
#include "../include/logging.h"   // Functii de logare folosite la raportarea erorilor
#include <stdio.h>                // Pentru snprintf()
#include <stdlib.h>               // Pentru atoi() si atof()
#include <string.h>               // Pentru strlen(), strcpy(), strncpy(), strchr(), strstr(), memmove()
#include <unistd.h>               // Pentru read(), write() si close()
#include <fcntl.h>                // Pentru open() si flag-ul O_RDONLY

// Elimina spatiile si caracterele inutile de la inceputul si sfarsitul unui sir.
// Functia modifica direct continutul stringului primit.
static void trim_config(char *str) {
    char *start = str;
    char *end;
    
    // Verificare de siguranta pentru pointer NULL
    if (!str) return;
    
    // Sare peste spatiile si tab-urile de la inceputul liniei
    while (*start == ' ' || *start == '\t') start++;
    
    // Daca sirul a ramas gol dupa eliminarea spatiilor, il setam explicit la string gol
    if (*start == '\0') {
        *str = '\0';
        return;
    }
    
    // Pozitionare pe ultimul caracter util din sir
    end = start + strlen(start) - 1;
    
    // Eliminare spatii, newline si carriage return de la final
    while (end > start && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) end--;
    *(end + 1) = '\0';
    
    // Daca textul util nu incepea de la pozitia initiala, mutam continutul la inceputul bufferului
    if (start != str) memmove(str, start, strlen(start) + 1);
}

// Citeste un rand din fisier caracter cu caracter pana la '\n' sau pana se umple bufferul.
// Intoarce numarul de caractere citite sau -1 la eroare / sfarsit de fisier.
static int read_config_line(int fd, char *buf, size_t size) {
    size_t i = 0;
    char c;
    
    // Citire caracter cu caracter pentru a construi linia curenta
    while (i < size - 1) {
        if (read(fd, &c, 1) <= 0) return -1;
        if (c == '\n') break;
        buf[i++] = c;
    }
    
    // Terminare corecta a sirului
    buf[i] = '\0';
    return i;
}

// Incarca valorile de configurare din fisier.
// Daca fisierul nu poate fi deschis, raman active valorile implicite.
int load_config(const char *filename, server_config_t *config) {
    int fd;
    char line[512];
    int in_geo = 0;
    int in_bbox = 0;

    // Initializare valori implicite pentru configuratia generala a serverului
    config->inet_port = 18081;
    strcpy(config->unix_socket, "/tmp/geods.sock");
    config->max_clients = 10;
    config->buffer_size = 4096;
    config->admin_timeout = 60;
    config->max_history = 100;
    config->max_points = 100000;
    config->max_segments = 1000;
    
    // Initializare valori implicite pentru sectiunea geo
    config->geo.earth_radius = 6371.0;
    config->geo.num_children = 4;
    strcpy(config->geo.temp_dir, "/tmp/geo_processing");
    strcpy(config->geo.uploads_dir, "processing/uploads");
    strcpy(config->geo.processing_dir, "processing/processing");
    strcpy(config->geo.outgoing_dir, "processing/outgoing");
    config->geo.bbox.min_lat = -90.0;
    config->geo.bbox.max_lat = 90.0;
    config->geo.bbox.min_lon = -180.0;
    config->geo.bbox.max_lon = 180.0;
    
    // Deschide fisierul de configurare doar pentru citire
    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        // Daca fisierul nu exista sau nu poate fi deschis, se folosesc valorile default
        log_message("[CONFIG] Cannot open config file, using defaults");
        return -1;
    }
    
    // Parcurge fisierul linie cu linie
    while (read_config_line(fd, line, sizeof(line)) >= 0) {
        char *p = line;
        char *eq;
        
        // Curata linia de spatii inutile
        trim_config(p);
        
        // Ignora liniile goale
        if (*p == '\0') continue;
        
        // Ignora liniile de comentariu
        if (*p == '#') continue;
        
        // Cauta si parseaza fiecare cheie de configurare cunoscuta
        
        if (strstr(p, "inet_port") != NULL) {
            eq = strchr(p, '=');
            if (eq) config->inet_port = atoi(eq + 1);
        }
        else if (strstr(p, "unix_socket") != NULL) {
            eq = strchr(p, '=');
            if (eq) {
                char *val = eq + 1;
                char *end;
                
                // Curata valoarea citita
                trim_config(val);
                
                // Elimina separatorul ';' daca exista
                end = strchr(val, ';');
                if (end) *end = '\0';
                
                // Copiaza valoarea in campul configuratiei in mod sigur
                strncpy(config->unix_socket, val, sizeof(config->unix_socket) - 1);
                config->unix_socket[sizeof(config->unix_socket) - 1] = '\0';
            }
        }
        else if (strstr(p, "max_clients") != NULL) {
            eq = strchr(p, '=');
            if (eq) config->max_clients = atoi(eq + 1);
        }
        else if (strstr(p, "buffer_size") != NULL) {
            eq = strchr(p, '=');
            if (eq) config->buffer_size = atoi(eq + 1);
        }
        else if (strstr(p, "admin_timeout") != NULL) {
            eq = strchr(p, '=');
            if (eq) config->admin_timeout = atoi(eq + 1);
        }
        else if (strstr(p, "max_history") != NULL) {
            eq = strchr(p, '=');
            if (eq) config->max_history = atoi(eq + 1);
        }
        else if (strstr(p, "max_points") != NULL) {
            eq = strchr(p, '=');
            if (eq) config->max_points = atoi(eq + 1);
        }
        else if (strstr(p, "max_segments") != NULL) {
            eq = strchr(p, '=');
            if (eq) config->max_segments = atoi(eq + 1);
        }
        else if (strstr(p, "geo:") != NULL) {
            // Marcheaza intrarea in sectiunea geo din fisierul de configurare
            in_geo = 1;
        }
        else if (strstr(p, "bbox:") != NULL && in_geo) {
            // Marcheaza intrarea in subsectiunea bbox din geo
            in_bbox = 1;
        }
        else if (in_geo && !in_bbox && strstr(p, "earth_radius") != NULL) {
            eq = strchr(p, '=');
            if (eq) config->geo.earth_radius = atof(eq + 1);
        }
        else if (in_geo && !in_bbox && strstr(p, "num_children") != NULL) {
            eq = strchr(p, '=');
            if (eq) config->geo.num_children = atoi(eq + 1);
        }
        else if (in_geo && !in_bbox && strstr(p, "temp_dir") != NULL) {
            eq = strchr(p, '=');
            if (eq) {
                char *val = eq + 1;
                char *end;
                trim_config(val);
                end = strchr(val, ';');
                if (end) *end = '\0';
                strncpy(config->geo.temp_dir, val, sizeof(config->geo.temp_dir) - 1);
                config->geo.temp_dir[sizeof(config->geo.temp_dir) - 1] = '\0';
            }
        }
        else if (in_geo && !in_bbox && strstr(p, "uploads_dir") != NULL) {
            eq = strchr(p, '=');
            if (eq) {
                char *val = eq + 1;
                char *end;
                trim_config(val);
                end = strchr(val, ';');
                if (end) *end = '\0';
                strncpy(config->geo.uploads_dir, val, sizeof(config->geo.uploads_dir) - 1);
                config->geo.uploads_dir[sizeof(config->geo.uploads_dir) - 1] = '\0';
            }
        }
        else if (in_geo && !in_bbox && strstr(p, "processing_dir") != NULL) {
            eq = strchr(p, '=');
            if (eq) {
                char *val = eq + 1;
                char *end;
                trim_config(val);
                end = strchr(val, ';');
                if (end) *end = '\0';
                strncpy(config->geo.processing_dir, val, sizeof(config->geo.processing_dir) - 1);
                config->geo.processing_dir[sizeof(config->geo.processing_dir) - 1] = '\0';
            }
        }
        else if (in_geo && !in_bbox && strstr(p, "outgoing_dir") != NULL) {
            eq = strchr(p, '=');
            if (eq) {
                char *val = eq + 1;
                char *end;
                trim_config(val);
                end = strchr(val, ';');
                if (end) *end = '\0';
                strncpy(config->geo.outgoing_dir, val, sizeof(config->geo.outgoing_dir) - 1);
                config->geo.outgoing_dir[sizeof(config->geo.outgoing_dir) - 1] = '\0';
            }
        }
        else if (in_geo && in_bbox && strstr(p, "min_lat") != NULL) {
            eq = strchr(p, '=');
            if (eq) config->geo.bbox.min_lat = atof(eq + 1);
        }
        else if (in_geo && in_bbox && strstr(p, "max_lat") != NULL) {
            eq = strchr(p, '=');
            if (eq) config->geo.bbox.max_lat = atof(eq + 1);
        }
        else if (in_geo && in_bbox && strstr(p, "min_lon") != NULL) {
            eq = strchr(p, '=');
            if (eq) config->geo.bbox.min_lon = atof(eq + 1);
        }
        else if (in_geo && in_bbox && strstr(p, "max_lon") != NULL) {
            eq = strchr(p, '=');
            if (eq) config->geo.bbox.max_lon = atof(eq + 1);
        }
        
        // Detecteaza inchiderea unei sectiuni din configuratie
        if (strchr(p, '}') != NULL) {
            if (in_bbox) in_bbox = 0;
            else if (in_geo) in_geo = 0;
        }
    }
    
    // Inchide descriptorul fisierului dupa terminarea citirii
    close(fd);
    
    return 0;
}

// Afiseaza in consola configuratia curenta a serverului.
// Se foloseste write() pentru a respecta stilul bazat pe apeluri de sistem.
void print_config(server_config_t *config) {
    char buf[512];
    int len;
    
    // Afisare antet configuratie
    write(STDOUT_FILENO, "=== CONFIGURATIE SERVER ===\n", 28);
    
    // Afisare campuri generale ale configuratiei
    len = snprintf(buf, sizeof(buf), "inet_port: %d\n", config->inet_port);
    write(STDOUT_FILENO, buf, len);
    
    len = snprintf(buf, sizeof(buf), "unix_socket: %s\n", config->unix_socket);
    write(STDOUT_FILENO, buf, len);
    
    len = snprintf(buf, sizeof(buf), "max_clients: %d\n", config->max_clients);
    write(STDOUT_FILENO, buf, len);
    
    len = snprintf(buf, sizeof(buf), "buffer_size: %d\n", config->buffer_size);
    write(STDOUT_FILENO, buf, len);
    
    len = snprintf(buf, sizeof(buf), "admin_timeout: %d\n", config->admin_timeout);
    write(STDOUT_FILENO, buf, len);
    
    len = snprintf(buf, sizeof(buf), "max_history: %d\n", config->max_history);
    write(STDOUT_FILENO, buf, len);
    
    len = snprintf(buf, sizeof(buf), "max_points: %d\n", config->max_points);
    write(STDOUT_FILENO, buf, len);
    
    len = snprintf(buf, sizeof(buf), "max_segments: %d\n", config->max_segments);
    write(STDOUT_FILENO, buf, len);
    
    // Afisare campuri din substructura geo
    len = snprintf(buf, sizeof(buf), "geo.earth_radius: %.2f\n", config->geo.earth_radius);
    write(STDOUT_FILENO, buf, len);
    
    len = snprintf(buf, sizeof(buf), "geo.num_children: %d\n", config->geo.num_children);
    write(STDOUT_FILENO, buf, len);
    
    len = snprintf(buf, sizeof(buf), "geo.temp_dir: %s\n", config->geo.temp_dir);
    write(STDOUT_FILENO, buf, len);
    
    len = snprintf(buf, sizeof(buf), "geo.uploads_dir: %s\n", config->geo.uploads_dir);
    write(STDOUT_FILENO, buf, len);
    
    len = snprintf(buf, sizeof(buf), "geo.processing_dir: %s\n", config->geo.processing_dir);
    write(STDOUT_FILENO, buf, len);
    
    len = snprintf(buf, sizeof(buf), "geo.outgoing_dir: %s\n", config->geo.outgoing_dir);
    write(STDOUT_FILENO, buf, len);
    
    // Afisare coordonate pentru bounding box-ul configurat
    len = snprintf(buf, sizeof(buf), "geo.bbox: [%.1f, %.1f, %.1f, %.1f]\n",
           config->geo.bbox.min_lat, config->geo.bbox.max_lat,
           config->geo.bbox.min_lon, config->geo.bbox.max_lon);
    write(STDOUT_FILENO, buf, len);
}