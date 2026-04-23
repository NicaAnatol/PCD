#include "../include/proto.h"
#include "../include/logging.h"
#include "../include/config.h"
#include <bits/getopt_core.h>

#define CONFIG_FILE "config/server.conf"


server_config_t g_config;

void *geo_worker_main(void *args) {
    (void)args;
    log_message("[WORKER] Starting geo worker thread");
    while (1) sleep(1);
    pthread_exit(NULL);
}

int main(int argc, char *argv[]) {
    pthread_t unixthr, inetthr, workerthr, queuethr;
    int opt;
    
    load_config(CONFIG_FILE, &g_config);
    
    while ((opt = getopt(argc, argv, "p:")) != -1) {
        if (opt == 'p') g_config.inet_port = atoi(optarg);
    }
    
    log_init("server.log");
    log_message("[MAIN] Starting Geo Processing Server...");
    
    print_config(&g_config);
    
    mkdir(g_config.geo.temp_dir, 0755);
    mkdir("processing/uploads", 0755);
    mkdir("processing/processing", 0755);
    mkdir("processing/outgoing", 0755);
    
    unlink(g_config.unix_socket);
    
    pthread_create(&unixthr, NULL, unix_main, (void *)g_config.unix_socket);
    pthread_create(&inetthr, NULL, inet_main, &g_config.inet_port);
    pthread_create(&workerthr, NULL, geo_worker_main, NULL);
    pthread_create(&queuethr, NULL, queue_processor, NULL);
    
    pthread_join(unixthr, NULL);
    pthread_join(inetthr, NULL);
    pthread_join(workerthr, NULL);
    pthread_join(queuethr, NULL);
    
    unlink(g_config.unix_socket);
    log_message("[MAIN] Server shutdown");
    
    return 0;
}