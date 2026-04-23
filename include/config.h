#ifndef CONFIG_H
#define CONFIG_H

typedef struct {
    int inet_port;
    char unix_socket[256];
    int max_clients;
    int buffer_size;
    int admin_timeout;
    int max_history;
    int max_points;
    int max_segments;
    
    struct {
        double earth_radius;
        int num_children;
        char temp_dir[256];
        char uploads_dir[256];
        char processing_dir[256];
        char outgoing_dir[256];
        struct {
            double min_lat;
            double max_lat;
            double min_lon;
            double max_lon;
        } bbox;
    } geo;
} server_config_t;

extern server_config_t g_config;

int load_config(const char *filename, server_config_t *config);
void print_config(server_config_t *config);

#endif