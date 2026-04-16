#include "../include/proto.h"
#include "../include/logging.h"
#include <bits/getopt_core.h>

#define UNIXSOCKET "/tmp/geods"
#define INETPORT   18081

pthread_mutex_t curmtx = PTHREAD_MUTEX_INITIALIZER;

void *geo_worker_main(void *args) {
    (void)args;
    log_message("[WORKER] Starting geo worker thread");
    while (1) sleep(1);
    pthread_exit(NULL);
}

int main(int argc, char *argv[]) {
    pthread_t unixthr, inetthr, workerthr;
    int iport = INETPORT;
    int opt;
    
    while ((opt = getopt(argc, argv, "p:")) != -1) {
        if (opt == 'p') iport = atoi(optarg);
    }
    
    log_init("server.log");
    log_message("[MAIN] Starting Geo Processing Server...");
    
    mkdir("processing", 0755);
    mkdir("processing/uploads", 0755);
    mkdir("processing/processing", 0755);
    mkdir("processing/outgoing", 0755);
    
    unlink(UNIXSOCKET);
    
    pthread_create(&unixthr, NULL, unix_main, (void *)UNIXSOCKET);
    pthread_create(&inetthr, NULL, inet_main, &iport);
    pthread_create(&workerthr, NULL, geo_worker_main, NULL);
    
    pthread_join(unixthr, NULL);
    pthread_join(inetthr, NULL);
    pthread_join(workerthr, NULL);
    
    unlink(UNIXSOCKET);
    log_message("[MAIN] Server shutdown");
    
    return 0;
}
