#include "../include/geo_proto.h"

/* Declaratii externe */
void *unix_main(void *args);
void *inet_main(void *args);
void *geo_worker_main(void *args);

#define UNIXSOCKET "/tmp/geods"
#define INETPORT   18081

pthread_mutex_t curmtx = PTHREAD_MUTEX_INITIALIZER;

int main(int argc, char *argv[]) {
    pthread_t unixthr, inetthr, workerthr;
    int iport;
    
    /* Parsare argumente linie comanda */
    int opt;
    char *config_file = "config/server.conf";
    
    while ((opt = getopt(argc, argv, "p:c:")) != -1) {
        switch (opt) {
            case 'p':
                iport = atoi(optarg);
                break;
            case 'c':
                config_file = optarg;
                break;
            default:
                break;
        }
    }
    
    /* Initialize logging */
    log_init("server.log");
    
    /* Incarca configuratia */
    if (load_config(config_file) < 0) {
        log_message("[ERROR] Cannot load config");
    }
    
    /* Creaza directoare necesare */
    mkdir("processing", 0755);
    mkdir("processing/uploads", 0755);
    mkdir("processing/processing", 0755);
    mkdir("processing/outgoing", 0755);
    
    /* Sterge socket-ul UNIX vechi */
    unlink(UNIXSOCKET);
    
    log_message("[MAIN] Starting Geo Processing Server...");
    
    /* Porneste thread-urile */
    pthread_create(&unixthr, NULL, unix_main, (void *)UNIXSOCKET);
    iport = INETPORT;
    pthread_create(&inetthr, NULL, inet_main, &iport);
    pthread_create(&workerthr, NULL, geo_worker_main, NULL);
    
    /* Asteapta thread-urile */
    pthread_join(unixthr, NULL);
    pthread_join(inetthr, NULL);
    pthread_join(workerthr, NULL);
    
    unlink(UNIXSOCKET);
    
    log_message("[MAIN] Server shutdown");
    
    return 0;
}

/* Thread worker pentru procesare background */
void *geo_worker_main(void *args) {
    log_message("[WORKER] Starting geo worker thread");
    
    while (1) {
        /* Verifica directorul processing/ pentru fisiere noi */
        /* inotify poate fi adaugat la nivel C */
        sleep(1);
    }
    
    pthread_exit(NULL);
}

/* Load config with libconfig */
int load_config(const char *config_file) {
    config_t cfg;
    config_init(&cfg);
    
    if (!config_read_file(&cfg, config_file)) {
        char err[256];
        int len = snprintf(err, sizeof(err),
                           "[CONFIG] Error reading %s\n", config_file);
        write(STDERR_FILENO, err, len);
        config_destroy(&cfg);
        return -1;
    }
    
    int port;
    if (config_lookup_int(&cfg, "server.inet_port", &port)) {
        char logbuf[128];
        int len = snprintf(logbuf, sizeof(logbuf),
                           "[CONFIG] INET port = %d\n", port);
        write(STDERR_FILENO, logbuf, len);
    }
    
    config_destroy(&cfg);
    return 0;
}