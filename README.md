# Proiect PCD - Aplicație Client-Server pentru Procesare Date Geospațiale

## Membri echipă
- M1: Nica Anatol - Connection Manager
- M2: Madalina Nechifor - Admin Client & Handler
- M3: Marton Andrada - Ordinary Client C & Handler
- M4: Maryna Oleksandr - Processing Queue & Handler

## Tema aleasă
Aplicație client-server pentru procesarea datelor geospațiale.  
Serverul primește puncte geografice din fișiere sau input direct și execută operații precum:
- calculul distanței totale
- simplificarea traseelor (Douglas-Peucker)
- filtrarea după bounding box (bbox)

Sistemul include:
- client obișnuit (CLI) în C și Python
- client admin ncurses
- server multi-threaded

## Nivel implementat
- [x] Nivel A
- [x] Nivel B
- [ ] Nivel C

## Funcționalități

### Server
- suport multi-client (socket INET + select)
- autentificare utilizatori (login/register)
- procesare date geospațiale
- coadă de task-uri (queue + thread + pipe notificare)
- statistici server
- blacklist IP și domeniu

### Operații geospațiale
- calcul distanță (Haversine / GEOS)
- simplificare traseu (Douglas-Peucker)
- filtrare bounding box
- distanță între două puncte
- afișare segmente traseu

### Client (C / Python)
- upload fișiere: CSV, GPX, GeoJSON
- upload_raw fișiere (chunked, 8192B)
- introducere manuală puncte
- status task, result task, cancel task
- download rezultat procesare (CSV)
- interfață CLI

### Admin 
- interfață cu meniu navigabil
- vizualizare statistici server
- listă clienți activi
- istoric comenzi
- coadă de procesare
- sesiuni active
- terminare sesiuni 
- blocare/deblocare IP
- blocare/deblocare domeniu
- anulare task 
- forțare deconectare client 

## Modificari pentru Milestone 2

### Transfer fișiere bidirectional
- `upload_raw` – upload fișier brut în chunk-uri de 8192 bytes (client - server)
- `download <task_id>` – descărcare fișier rezultat CSV (server - client)

### Procesare asincronă
- returnare imediată `task_id` la upload
- interogare status: `status <task_id>`
- obținere rezultate: `result <task_id>`

### Persistență task-uri
- task-uri finalizate mutate în `completed_head`
- păstrare 5 minute, cleanup automat

### Blacklist
- `BLOCK_IP <ip>` / `UNBLOCK_IP <ip>`
- `BLOCK_DOMAIN <domain>` / `UNBLOCK_DOMAIN <domain>`

### Administrare
- `CANCEL <task_id>` – anulare task aflat în așteptare/procesare
- `FORCE_DISCONNECT <session_id>` – închidere forțată socket client

### Protocol
- câmp `requestID` în antet pentru corelare cerere-răspuns
- clientul verifică potrivirea ID-urilor

### Client admin
- citire completă răspunsuri în `send_command()` 

### Generare fișier rezultat
- CSV generat în `processing/outgoing/task_X_result.csv`
- format: `lat,lon,distance_to_next_km`

## Tehnologii utilizate
- C (POSIX)
- socket-uri INET și UNIX
- pthreads (thread-uri, mutex, cond)
- pipe anonim (notificare coadă)
- fork()/wait() (procese)
- ncurses (interfață admin)
- GEOS (operații geospațiale, opțional)

## Compilare și rulare
```bash
make
make admin_client
make ordinary_client
