#include "../include/geo_proto.h"

void init_sockaddr(struct sockaddr_in *name, const char *hostname, uint16_t port) {
    struct hostent *hostinfo;
    name->sin_family = AF_INET;
    name->sin_port = htons(port);
    hostinfo = gethostbyname(hostname);
    if (hostinfo == NULL) {
        char err[] = "Unknown host\n";
        write(STDERR_FILENO, err, sizeof(err)-1);
        exit(EXIT_FAILURE);
    }
    name->sin_addr = *(struct in_addr *)hostinfo->h_addr;
}

int main(int argc, char *argv[]) {
    int sock;
    struct sockaddr_in servername;
    char *server_host = "127.0.0.1";
    int server_port = 18081;
    
    /* Parsare argumente */
    if (argc > 1) server_host = argv[1];
    if (argc > 2) server_port = atoi(argv[2]);
    
    /* Creare socket */
    sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        char err[] = "socket failed\n";
        write(STDERR_FILENO, err, sizeof(err)-1);
        exit(EXIT_FAILURE);
    }
    
    /* Conectare */
    init_sockaddr(&servername, server_host, server_port);
    if (connect(sock, (struct sockaddr *)&servername, sizeof(servername)) < 0) {
        char err[] = "connect failed\n";
        write(STDERR_FILENO, err, sizeof(err)-1);
        exit(EXIT_FAILURE);
    }
    
    /* Obține client ID */
    msgHeaderType h;
    msgIntType m;
    h.clientID = 0;
    h.opID = 0;
    writeSingleInt(sock, h, 0);
    readSingleInt(sock, &m);
    int clientID = m.msg;
    
    char logbuf[128];
    int len = snprintf(logbuf, sizeof(logbuf), "Got client ID: %d\n", clientID);
    write(STDOUT_FILENO, logbuf, len);
    
    /* Upload fisier cu puncte geo */
    h.clientID = clientID;
    h.opID = OPR_UPLOAD_GEO;
    
    char filename[] = "test_points.txt";
    writeSingleString(sock, h, filename);
    
    /* Trimite punctele */
    pointMsgType points[] = {
        {44.4268, 26.1025},  /* Bucuresti */
        {45.7489, 21.2087},  /* Timisoara */
        {46.7712, 23.6236},  /* Cluj */
        {44.3333, 23.8167},  /* Craiova */
        {45.2692, 27.9575}   /* Braila */
    };
    int point_count = 5;
    
    writeSingleInt(sock, h, point_count);
    send(sock, points, sizeof(pointMsgType) * point_count, 0);
    
    /* Primeste rezultat */
    geoStatsMsgType stats;
    recv(sock, &stats, sizeof(stats), 0);
    
    len = snprintf(logbuf, sizeof(logbuf),
                   "\n=== REZULTAT ===\n"
                   "Puncte: %d\n"
                   "Distanta totala: %.2f km\n"
                   "Timp procesare: %ld ms\n",
                   stats.point_count, stats.total_distance, stats.processing_time_ms);
    write(STDOUT_FILENO, logbuf, len);
    
    /* Inchidere */
    h.opID = OPR_BYE;
    writeSingleInt(sock, h, 0);
    close(sock);
    
    return 0;
}