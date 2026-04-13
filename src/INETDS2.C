#include "../include/geo_proto.h"
#include "../include/proto.h"

int inet_socket(uint16_t port, short reuse) {
    int sock;
    struct sockaddr_in name;
    
    sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        pthread_exit(NULL);
    }
    
    if (reuse) {
        int reuseAddrON = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuseAddrON, sizeof(reuseAddrON));
    }
    
    name.sin_family = AF_INET;
    name.sin_port = htons(port);
    name.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&name, sizeof(name)) < 0) {
        pthread_exit(NULL);
    }
    
    return sock;
}

long create_client_id(void) {
    time_t rawtime;
    time(&rawtime);
    return (long)rawtime;
}

void *inet_main(void *args) {
    int port = *((int *)args);
    int sock;
    size_t size;
    fd_set active_fd_set, read_fd_set;
    struct sockaddr_in clientname;
    
    log_message("[INET] Starting INET server...");
    
    if ((sock = inet_socket(port, 1)) < 0) {
        pthread_exit(NULL);
    }
    if (listen(sock, 10) < 0) {
        pthread_exit(NULL);
    }
    
    FD_ZERO(&active_fd_set);
    FD_SET(sock, &active_fd_set);
    
    while (1) {
        int i;
        read_fd_set = active_fd_set;
        
        if (select(FD_SETSIZE, &read_fd_set, NULL, NULL, NULL) < 0) {
            pthread_exit(NULL);
        }
        
        for (i = 0; i < FD_SETSIZE; ++i) {
            if (FD_ISSET(i, &read_fd_set)) {
                if (i == sock) {
                    /* Conexiune noua */
                    int new_fd;
                    size = sizeof(clientname);
                    new_fd = accept(sock, (struct sockaddr *)&clientname, (socklen_t *)&size);
                    if (new_fd < 0) {
                        continue;
                    }
                    FD_SET(new_fd, &active_fd_set);
                    
                    char logbuf[128];
                    int len = snprintf(logbuf, sizeof(logbuf),
                                       "[INET] New connection on fd=%d\n", new_fd);
                    write(STDERR_FILENO, logbuf, len);
                }
                else {
                    /* Date de la un client existent */
                    msgHeaderType h = peekMsgHeader(i);
                    int clientID = h.clientID;
                    
                    if (clientID < 0) {
                        close(i);
                        FD_CLR(i, &active_fd_set);
                        continue;
                    }
                    
                    if (clientID == 0) {
                        /* Client nou - da-i un ID */
                        int newID = create_client_id();
                        msgIntType m;
                        readSingleInt(i, &m);
                        writeSingleInt(i, h, newID);
                        
                        char logbuf[128];
                        int len = snprintf(logbuf, sizeof(logbuf),
                                           "[INET] New client assigned ID=%d\n", newID);
                        write(STDERR_FILENO, logbuf, len);
                    }
                    else {
                        int operation = h.opID;
                        
                        switch (operation) {
                            case OPR_UPLOAD_GEO: {
                                /* Upload fisier cu puncte geo */
                                msgStringType filename;
                                if (readSingleString(i, &filename) < 0) {
                                    close(i);
                                    FD_CLR(i, &active_fd_set);
                                    break;
                                }
                                
                                char filepath[256];
                                snprintf(filepath, sizeof(filepath), "processing/uploads/%d_%s",
                                         clientID, filename.msg);
                                
                                int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                                if (fd >= 0) {
                                    pointMsgType *points = NULL;
                                    int point_count = 0;
                                    
                                    if (readGeoPoints(i, &points, &point_count) >= 0) {
                                        /* Salveaza punctele in fisier */
                                        char buf[128];
                                        for (int p = 0; p < point_count; p++) {
                                            int len = snprintf(buf, sizeof(buf), "%.6f,%.6f\n",
                                                               points[p].lat, points[p].lon);
                                            write(fd, buf, len);
                                        }
                                        close(fd);
                                        
                                        /* Procesare cu fork/wait */
                                        geoStatsMsgType stats;
                                        process_with_children(filepath, 4, &stats);
                                        
                                        /* Trimite rezultatul */
                                        writeGeoStats(i, h, &stats);
                                        
                                        free(points);
                                    }
                                }
                                free(filename.msg);
                                break;
                            }
                            
                            case OPR_GET_DISTANCE: {
                                /* Calculeaza distanta */
                                msgStringType filename;
                                if (readSingleString(i, &filename) < 0) break;
                                
                                char filepath[256];
                                snprintf(filepath, sizeof(filepath), "processing/uploads/%d_%s",
                                         clientID, filename.msg);
                                
                                geoStatsMsgType stats;
                                process_with_children(filepath, 4, &stats);
                                writeGeoStats(i, h, &stats);
                                
                                free(filename.msg);
                                break;
                            }
                            
                            case OPR_BYE:
                            default:
                                close(i);
                                FD_CLR(i, &active_fd_set);
                                break;
                        }
                    }
                }
            }
        }
    }
    
    pthread_exit(NULL);
}