#include "../include/logging.h"   // Pentru functiile de logare, daca sunt necesare in acest modul
#include "../include/proto.h"     // Pentru structurile de mesaje si functiile/prototipurile protocolului

// Citeste fara a consuma din socket header-ul urmatorului mesaj.
// Se foloseste MSG_PEEK pentru a inspecta header-ul inainte de prelucrarea efectiva.
msgHeaderType peekMsgHeader(int sock) {
    ssize_t nb;
    char full_header[16];  // 4 int-uri = 16 bytes
    msgHeaderType h;
    
    // Initializare preventiva
    memset(&h, 0, sizeof(h));
    
    // Citeste header-ul complet de 16 bytes din socket fara a-l elimina din buffer
    nb = recv(sock, full_header, sizeof(full_header), MSG_PEEK | MSG_WAITALL);
    
    if (nb != sizeof(full_header)) {
        h.opID = -1;
        h.clientID = -1;
        return h;
    }
    
    int *parts = (int*)full_header;
    h.msgSize = ntohl(parts[0]);
    h.clientID = ntohl(parts[1]);
    h.opID = ntohl(parts[2]);
    h.requestID = ntohl(parts[3]);
    
    return h;
}

// Citeste din socket un mesaj care contine un singur int.
// Valoarea este extrasa din structura completa si convertita in host byte order.
int readSingleInt(int sock, msgIntType *m) {
    ssize_t nb;
    char buffer[20];  // 16 bytes header + 4 bytes payload
    int received = 0;
    
    // Citeste intregul mesaj (header 16 + payload 4)
    while (received < 20) {
        nb = recv(sock, buffer + received, 20 - received, MSG_WAITALL);
        if (nb <= 0) {
            m->msg = -1;
            return -1;
        }
        received += nb;
    }
    
    // Extrage valoarea (ultimii 4 bytes)
    m->msg = ntohl(((int*)buffer)[4]);
    return received;
}

// Scrie in socket un mesaj complet care contine un header si un int.
// Header-ul are 4 int-uri: msgSize, clientID, opID, requestID (16 bytes)
// Urmeaza payload-ul de 4 bytes
int writeSingleInt(int sock, msgHeaderType h, int i) {
    char buffer[20];  // 16 bytes header + 4 bytes payload
    int *parts = (int*)buffer;
    
    parts[0] = htonl(16);           // msgSize
    parts[1] = htonl(h.clientID);   // clientID
    parts[2] = htonl(h.opID);       // opID
    parts[3] = htonl(h.requestID);  // requestID
    parts[4] = htonl(i);            // payload
    
    ssize_t nb = send(sock, buffer, sizeof(buffer), 0);
    if (nb == -1 || nb != sizeof(buffer)) return -1;
    return nb;
}

// Citeste din socket un string.
// Protocolul folosit este: header 16 bytes, apoi lungimea stringului (4 bytes), apoi continutul.
int readSingleString(int sock, msgStringType *str) {
    char header[16];
    ssize_t nb;
    int received = 0;
    
    // Citeste header-ul de 16 bytes
    while (received < 16) {
        nb = recv(sock, header + received, 16 - received, MSG_WAITALL);
        if (nb <= 0) return -1;
        received += nb;
    }
    
    // Citeste lungimea stringului (4 bytes)
    char len_buf[4];
    received = 0;
    while (received < 4) {
        nb = recv(sock, len_buf + received, 4 - received, MSG_WAITALL);
        if (nb <= 0) return -1;
        received += nb;
    }
    
    int strSize = ntohl(*(int*)len_buf);
    
    // Validare simpla pentru a evita dimensiuni invalide sau excesive
    if (strSize <= 0 || strSize > 65536) return -1;
    
    // Aloca memorie pentru string + terminatorul de sir
    str->msg = malloc(strSize + 1);
    if (!str->msg) return -1;
    
    // Citeste continutul efectiv al stringului
    received = 0;
    while (received < strSize) {
        nb = recv(sock, str->msg + received, strSize - received, MSG_WAITALL);
        if (nb <= 0) {
            free(str->msg);
            str->msg = NULL;
            return -1;
        }
        received += nb;
    }
    
    // Adauga terminator de sir pentru a putea trata datele ca string C
    str->msg[received] = '\0';
    return received;
}

// Scrie in socket un string.
// Protocolul folosit este: header 16 bytes, apoi lungimea stringului (4 bytes), apoi continutul.
int writeSingleString(int sock, msgHeaderType h, char *str) {
    int strSize = strlen(str);
    int total_len = 16 + 4 + strSize;  // header 16 + length 4 + content
    char *buffer = malloc(total_len);
    if (!buffer) return -1;
    
    int *parts = (int*)buffer;
    parts[0] = htonl(total_len);
    parts[1] = htonl(h.clientID);
    parts[2] = htonl(h.opID);
    parts[3] = htonl(h.requestID);
    parts[4] = htonl(strSize);
    
    memcpy(buffer + 20, str, strSize);
    
    ssize_t nb = send(sock, buffer, total_len, 0);
    free(buffer);
    
    if (nb == -1 || nb != total_len) return -1;
    return nb;
}