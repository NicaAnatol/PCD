
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <signal.h>

#define UNIX_SOCKET_PATH "/tmp/geods"
#define BUFFER_SIZE 8192
#define TIMEOUT_SEC 60

static int sock = -1;
static volatile int timeout_counter = 0;
static WINDOW *main_wnd, *stats_wnd;

int connect_to_server(void) {
    int s;
    struct sockaddr_un addr;
    
    s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return -1;
    
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, UNIX_SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

int send_command(const char *cmd, char *response, size_t resp_size) {
    if (sock < 0) return -1;
    
    write(sock, cmd, strlen(cmd));
    
    struct timespec req = {0, 100000000L};
    struct timespec rem;
    nanosleep(&req, &rem);
    
    ssize_t bytes = read(sock, response, resp_size - 1);
    if (bytes > 0) {
        response[bytes] = '\0';
        return bytes;
    }
    return -1;
}

void format_stats(char *buffer, size_t size) {
    char response[BUFFER_SIZE];
    if (send_command("STATS", response, sizeof(response)) < 0) {
        snprintf(buffer, size, "Eroare la conectare\n");
        return;
    }
    
    char *line = strtok(response, "\n");
    int line_num = 0;
    buffer[0] = '\0';
    
    while (line && line_num < 20) {
        if (strstr(line, "=== STATISTICI") && line_num == 0) {
            strcat(buffer, "┌─────────────────────────────────────────┐\n");
        }
        if (strstr(line, "========================")) {
            strcat(buffer, "└─────────────────────────────────────────┘\n");
        } else {
            strcat(buffer, "│ ");
            strcat(buffer, line);
            int len = strlen(line);
            while (len++ < 39) strcat(buffer, " ");
            strcat(buffer, "│\n");
        }
        line = strtok(NULL, "\n");
        line_num++;
    }
}

void format_clients(char *buffer, size_t size) {
    char response[BUFFER_SIZE];
    if (send_command("CLIENTS", response, sizeof(response)) < 0) {
        snprintf(buffer, size, "Eroare la obtinerea listei clientilor\n");
        return;
    }
    
    buffer[0] = '\0';
    strcat(buffer, "┌─────────────────────────────────────────┐\n");
    strcat(buffer, "│         CLIENTI CONECTATI               │\n");
    strcat(buffer, "├─────────────────────────────────────────┤\n");
    
    char *line = strtok(response, "\n");
    while (line) {
        strcat(buffer, "│ ");
        strcat(buffer, line);
        int len = strlen(line);
        while (len++ < 39) strcat(buffer, " ");
        strcat(buffer, "│\n");
        line = strtok(NULL, "\n");
    }
    strcat(buffer, "└─────────────────────────────────────────┘\n");
}

void format_history(char *buffer, size_t size) {
    char response[BUFFER_SIZE];
    if (send_command("HISTORY", response, sizeof(response)) < 0) {
        snprintf(buffer, size, "Eroare la obtinerea istoricului\n");
        return;
    }
    
    buffer[0] = '\0';
    strcat(buffer, "┌─────────────────────────────────────────┐\n");
    strcat(buffer, "│         ISTORIC COMENZI                 │\n");
    strcat(buffer, "├─────────────────────────────────────────┤\n");
    
    char *line = strtok(response, "\n");
    while (line) {
        strcat(buffer, "│ ");
        strcat(buffer, line);
        int len = strlen(line);
        while (len++ < 39) strcat(buffer, " ");
        strcat(buffer, "│\n");
        line = strtok(NULL, "\n");
    }
    strcat(buffer, "└─────────────────────────────────────────┘\n");
}

void format_queue(char *buffer, size_t size) {
    char response[BUFFER_SIZE];
    if (send_command("QUEUE", response, sizeof(response)) < 0) {
        snprintf(buffer, size, "Eroare la obtinerea cozii\n");
        return;
    }
    
    buffer[0] = '\0';
    strcat(buffer, "┌─────────────────────────────────────────┐\n");
    strcat(buffer, "│         COADA DE PROCESARE              │\n");
    strcat(buffer, "├─────────────────────────────────────────┤\n");
    
    char *line = strtok(response, "\n");
    while (line) {
        strcat(buffer, "│ ");
        strcat(buffer, line);
        int len = strlen(line);
        while (len++ < 39) strcat(buffer, " ");
        strcat(buffer, "│\n");
        line = strtok(NULL, "\n");
    }
    strcat(buffer, "└─────────────────────────────────────────┘\n");
}

/* Formateaza timp mediu */
void format_avg_time(char *buffer, size_t size) {
    char response[BUFFER_SIZE];
    if (send_command("AVG_TIME", response, sizeof(response)) < 0) {
        snprintf(buffer, size, "Eroare la obtinerea timpului mediu\n");
        return;
    }
    
    buffer[0] = '\0';
    strcat(buffer, "┌─────────────────────────────────────────┐\n");
    strcat(buffer, "│         TIMP MEDIU PROCESARE            │\n");
    strcat(buffer, "├─────────────────────────────────────────┤\n");
    strcat(buffer, "│ ");
    strcat(buffer, response);
    int len = strlen(response);
    while (len++ < 39) strcat(buffer, " ");
    strcat(buffer, "│\n");
    strcat(buffer, "└─────────────────────────────────────────┘\n");
}

/* Initializare culori ncurses */
void init_colors(void) {
    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_RED, COLOR_BLACK);
        init_pair(2, COLOR_GREEN, COLOR_BLACK);
        init_pair(3, COLOR_YELLOW, COLOR_BLACK);
        init_pair(4, COLOR_CYAN, COLOR_BLACK);
        init_pair(5, COLOR_WHITE, COLOR_BLUE);
    }
}

void draw_interface(int selected) {
    char buffer[BUFFER_SIZE];
    int row = 2;
    
    werase(main_wnd);
    box(main_wnd, 0, 0);
    
    wattron(main_wnd, COLOR_PAIR(5) | A_BOLD);
    mvwprintw(main_wnd, 0, 2, "=== ADMIN - MONITOR SERVER GEO ===");
    wattroff(main_wnd, COLOR_PAIR(5) | A_BOLD);
    
    mvwprintw(main_wnd, 0, 35, "Timeout: %2d sec", TIMEOUT_SEC - timeout_counter);
    
    const char *menu[] = {
        "1. Statistici generale",
        "2. Lista clienti conectati",
        "3. Coada de procesare",
        "4. Istoric comenzi",
        "5. Timp mediu procesare",
        "6. Creare proces (fork test)",
        "q. Iesire"
    };
    int n_menu = sizeof(menu) / sizeof(menu[0]);
    
    mvwprintw(main_wnd, row++, 2, "=== MENIU ===");
    row++;
    
    for (int i = 0; i < n_menu; i++) {
        if (i == selected) {
            wattron(main_wnd, A_REVERSE);
            mvwprintw(main_wnd, row + i, 4, "%s", menu[i]);
            wattroff(main_wnd, A_REVERSE);
        } else {
            mvwprintw(main_wnd, row + i, 4, "%s", menu[i]);
        }
    }
    
    switch (selected) {
        case 0:
            format_stats(buffer, sizeof(buffer));
            break;
        case 1:
            format_clients(buffer, sizeof(buffer));
            break;
        case 2:
            format_queue(buffer, sizeof(buffer));
            break;
        case 3:
            format_history(buffer, sizeof(buffer));
            break;
        case 4:
            format_avg_time(buffer, sizeof(buffer));
            break;
        default:
            buffer[0] = '\0';
            break;
    }
    
    werase(stats_wnd);
    box(stats_wnd, 0, 0);
    mvwprintw(stats_wnd, 0, 2, "DATE");
    
    int line = 1;
    char *line_str = strtok(buffer, "\n");
    while (line_str && line < getmaxy(stats_wnd) - 1) {
        mvwprintw(stats_wnd, line++, 2, "%s", line_str);
        line_str = strtok(NULL, "\n");
    }
    
    wrefresh(main_wnd);
    wrefresh(stats_wnd);
}

int main(void) {
    int selected = 0;
    int ch;
    int running = 1;
    time_t last_activity;
    
    sock = connect_to_server();
    if (sock < 0) {
        write(STDERR_FILENO, "Eroare: Nu se poate conecta la server.\n", 39);
        return 1;
    }
    
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    timeout(100);
    
    init_colors();
    
    int max_y = LINES - 2;
    int max_x = COLS - 2;
    
    main_wnd = newwin(max_y, 30, 1, 1);
    stats_wnd = newwin(max_y, max_x - 30, 1, 32);
    
    scrollok(stats_wnd, TRUE);
    
    last_activity = time(NULL);
    timeout_counter = 0;
    
    while (running) {
        draw_interface(selected);
        
        if (time(NULL) - last_activity > TIMEOUT_SEC) {
            running = 0;
            break;
        }
        
        ch = getch();
        if (ch != ERR) {
            last_activity = time(NULL);
            timeout_counter = 0;
            
            switch (ch) {
                case '1':
                    selected = 0;
                    break;
                case '2':
                    selected = 1;
                    break;
                case '3':
                    selected = 2;
                    break;
                case '4':
                    selected = 3;
                    break;
                case '5':
                    selected = 4;
                    break;
                case '6':
                    send_command("PROCESSES", NULL, 0);
                    break;
                case 'q':
                case 'Q':
                    running = 0;
                    break;
                case KEY_UP:
                    if (selected > 0) selected--;
                    break;
                case KEY_DOWN:
                    if (selected < 5) selected++;
                    break;
            }
        }
        
        timeout_counter = time(NULL) - last_activity;
    }
    
    send_command("EXIT", NULL, 0);
    close(sock);
    endwin();
    
    write(STDOUT_FILENO, "Admin deconectat.\n", 18);
    return 0;
}
