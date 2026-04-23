#include "../include/logging.h"   // Pentru functiile de logare, daca sunt necesare in acest modul
#include "../include/proto.h"     // Pentru structurile de mesaje si functiile/prototipurile protocolului

// Citeste fara a consuma din socket header-ul urmatorului mesaj.
// Se foloseste MSG_PEEK pentru a inspecta header-ul inainte de prelucrarea efectiva.
msgHeaderType peekMsgHeader(int sock) {
    ssize_t nb;
    msgHeaderType h;
    
    // Initializare preventiva a dimensiunii header-ului
    h.msgSize = htonl(sizeof(h));
    
    // Citeste header-ul din socket fara a-l elimina din buffer
    nb = recv(sock, &h, sizeof(h), MSG_PEEK | MSG_WAITALL);
    
    // Conversie din network byte order in host byte order
    h.msgSize = ntohl(h.msgSize);
    h.clientID = ntohl(h.clientID);
    h.opID = ntohl(h.opID);
    
    // Daca recv a esuat sau conexiunea a fost inchisa, se marcheaza header-ul ca invalid
    if (nb == -1 || nb == 0) {
        h.opID = h.clientID = -1;
    }
    
    return h;
}

// Citeste din socket un mesaj care contine un singur int.
// Valoarea este extrasa din structura completa si convertita in host byte order.
int readSingleInt(int sock, msgIntType *m) {
    ssize_t nb;
    singleIntMsgType s;
    
    // Citeste intregul mesaj format din header + int
    nb = recv(sock, &s, sizeof(s), MSG_WAITALL);
    
    if (nb <= 0) {
        // In caz de eroare sau inchidere conexiune, mesajul este marcat invalid
        m->msg = -1;
        return -1;
    }
    
    // Extrage si converteste valoarea intreaga primita
    m->msg = ntohl(s.i.msg);
    return nb;
}

// Scrie in socket un mesaj complet care contine un header si un int.
int writeSingleInt(int sock, msgHeaderType h, int i) {
    singleIntMsgType s;
    
    // Completeaza campurile mesajului in network byte order
    s.header.clientID = htonl(h.clientID);
    s.header.opID = htonl(h.opID);
    s.i.msg = htonl(i);
    s.header.msgSize = htonl(sizeof(s));
    
    // Trimite structura completa prin socket
    ssize_t nb = send(sock, &s, sizeof(s), 0);
    if (nb == -1 || nb == 0) return -1;
    return nb;
}

// Citeste din socket un string.
// Protocolul folosit este: mai intai se citeste lungimea stringului, apoi continutul propriu-zis.
int readSingleString(int sock, msgStringType *str) {
    msgIntType m;
    
    // Citeste mai intai dimensiunea stringului
    ssize_t nb = readSingleInt(sock, &m);
    if (nb < 0) return -1;
    
    int strSize = m.msg;
    
    // Validare simpla pentru a evita dimensiuni invalide sau excesive
    if (strSize <= 0 || strSize > 65536) return -1;
    
    // Aloca memorie pentru string + terminatorul de sir
    str->msg = malloc(strSize + 1);
    if (!str->msg) return -1;
    
    // Citeste continutul efectiv al stringului
    nb = recv(sock, str->msg, strSize, MSG_WAITALL);
    if (nb <= 0) {
        free(str->msg);
        str->msg = NULL;
        return -1;
    }
    
    // Adauga terminator de sir pentru a putea trata datele ca string C
    str->msg[nb] = '\0';
    return nb;
}

// Scrie in socket un string.
// Protocolul folosit este: mai intai se trimite lungimea stringului, apoi continutul acestuia.
int writeSingleString(int sock, msgHeaderType h, char *str) {
    int strSize = strlen(str);
    
    // Trimite mai intai dimensiunea stringului
    ssize_t nb = writeSingleInt(sock, h, strSize);
    if (nb < 0) return -1;
    
    // Trimite apoi continutul propriu-zis
    nb = send(sock, str, strSize, 0);
    if (nb == -1 || nb == 0) return -1;
    return nb;
}