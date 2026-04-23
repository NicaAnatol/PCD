# Proiect PCD - Aplicatie Client-Server

## Membri echipa
- M1: [Nica Anatol] - Connection Manager
- M2: [Madalina Nechifor] - Admin Client & Handler
- M3: [Marton Andrada] - Ordinary Client C & Handler
- M4: [Maryna Oleksandr] - Processing Queue & Handler

## Tema aleasa
Aplicatie client-server pentru procesarea datelor geospatiale.  
Serverul primeste puncte geografice (din fisiere sau input direct) si executa operatii precum:
- calculul distantei totale
- simplificarea traseelor
- filtrarea dupa bounding box (bbox)

Sistemul include:
- client obisnuit (CLI)
- client admin (interfata ncurses)
- server multi-client

## Nivel implementat
- [x] Nivel A
- [ ] Nivel B
- [ ] Nivel C

## Functionalitati

### Server
- suport multi-client (socket INET + select)
- autentificare utilizatori (login/register)
- procesare date geospatiale
- coada de task-uri (queue + thread)
- statistici server

### Operatii geospatiale
- calcul distanta (Haversine / GEOS)
- simplificare traseu (Douglas-Peucker)
- filtrare bounding box
- distanta intre doua puncte
- afisare segmente traseu

### Client
- upload fisiere:
  - CSV
  - GPX
  - GeoJSON
- introducere manuala puncte
- interfata CLI

### Admin
- interfata ncurses
- vizualizare statistici server
- lista clienti activi
- istoric comenzi
- coada de procesare
- sesiuni active
- terminare sesiuni (KILL)

## Tehnologii utilizate
- C (POSIX)
- socket-uri INET si UNIX
- pthreads (thread-uri)
- fork()/wait() (procese)
- ncurses (interfata admin)
- GEOS (operatii geospatiale)

## Compilare

```bash
make
make admin_client
make ordinary_client