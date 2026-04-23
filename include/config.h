#ifndef CONFIG_H
#define CONFIG_H

// Structura principala de configurare pentru server
typedef struct {
    int inet_port;           // portul pentru conexiuni TCP/IP (INET)
    char unix_socket[256];   // path-ul pentru socket Unix
    int max_clients;         // numarul maxim de clienti conectati simultan
    int buffer_size;         // dimensiunea bufferului pentru comunicatie
    int admin_timeout;       // timeout pentru operatiuni administrative
    int max_history;         // numar maxim de elemente in istoric
    int max_points;          // numar maxim de puncte procesate
    int max_segments;        // numar maxim de segmente

    // Sub-structura pentru configurari geografice si procesare
    struct {
        double earth_radius;     // raza Pamantului folosita in calcule geografice
        int num_children;        // numar de procese copil (pentru fork)
        
        // Directoare folosite in procesare fisiere
        char temp_dir[256];      // director temporar
        char uploads_dir[256];   // director pentru fisiere incarcate
        char processing_dir[256];// director pentru fisiere in procesare
        char outgoing_dir[256];  // director pentru rezultate finale

        // Bounding box (zona geografica de interes)
        struct {
            double min_lat;  // latitudine minima
            double max_lat;  // latitudine maxima
            double min_lon;  // longitudine minima
            double max_lon;  // longitudine maxima
        } bbox;
    } geo;
} server_config_t;

// Variabila globala de configurare accesibila in tot programul
extern server_config_t g_config;

// Functie pentru incarcarea configuratiei din fisier
int load_config(const char *filename, server_config_t *config);

// Functie pentru afisarea configuratiei (debug)
void print_config(server_config_t *config);

#endif