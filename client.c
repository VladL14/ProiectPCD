/**
 * Echipa 8
 * IR3 2026
 * Proiect PCD - Client
 * Rezumat: client TCP care se conecteaza la server si ofera un meniu
 * interactiv pentru gestionarea dependintelor necesare generarii unui Dockerfile.
 * Utilizatorul poate adauga, modifica, sterge si vizualiza dependintele.
 * La comanda de generare, trimite lista la server si primeste Dockerfile-ul.
 * Am tratat urmatoarele situatii cu erori:
 * 1. erori la socket/connect
 * 2. input invalid in meniu (optiune necunoscuta, index in afara listei)
 * 3. lista plina sau goala
 * 4. erori la citire/scriere fisier output
 * 5. conexiune pierduta in timpul primirii Dockerfile-ului
 */

#include <arpa/inet.h>           // pt inet_addr
#include <errno.h>               // pt errno
#include <fcntl.h>               // pt open, O_WRONLY, O_CREAT, O_TRUNC
#include <netinet/in.h>          // pt sockaddr_in
#include <stddef.h>              // pt NULL, size_t
#include <stdlib.h>              // pt strtol, EXIT_SUCCESS, EXIT_FAILURE
#include <string.h>              // pt strcmp, strstr, memset
#include <sys/_endian.h>         // pt htons (macOS)
#include <sys/_types/_ssize_t.h> // pt ssize_t
#include <sys/fcntl.h>           // pt O_WRONLY, O_CREAT, O_TRUNC (macOS)
#include <sys/socket.h>          // pt socket, connect, send, recv
#include <unistd.h>              // pt read, write, close, STDIN_FILENO, STDOUT_FILENO

#define MAX_DEPS 10
#define DEP_NAME_LEN 64
#define INPUT_BUF_SIZE 128
#define PAYLOAD_BUF_SIZE 1024
#define RECV_BUF_SIZE 1024
#define SERVER_PORT 8080
#define DECIMAL_BASE 10
#define OUTPUT_FILE_MODE 0644
#define DEP_ENTRY_OVERHEAD 4 /* 'D' + ':' + spatiu + marja siguranta */

/* lista de dependinte gestionata prin meniu */
static char dep_list[MAX_DEPS][DEP_NAME_LEN];
static int dep_count = 0;

/* calculeaza lungimea unui string */
static size_t custom_len(const char *str) {
    size_t idx = 0;
    while (str[idx] != '\0') {
        idx++;
    }
    return idx;
}

/* scrie un text pe stdout */
static void write_str(const char *text) {
    (void)write(STDOUT_FILENO, text, custom_len(text));
}

/* scrie un numar pozitiv pe stdout (suficient pt 1..MAX_DEPS) */
static void write_int(int val) {
    if (val >= DECIMAL_BASE) {
        char tens = (char)('0' + (val / DECIMAL_BASE));
        (void)write(STDOUT_FILENO, &tens, 1);
    }
    char units = (char)('0' + (val % DECIMAL_BASE));
    (void)write(STDOUT_FILENO, &units, 1);
}

/* citeste o linie de la tastatura, elimina newline-ul de la final */
static int read_input(char *buf, size_t buf_size) {
    if (buf_size < 2U) {
        return 0;
    }
    ssize_t raw = read(STDIN_FILENO, buf, buf_size - 1);
    if (raw <= 0) {
        return 0;
    }
    size_t len = (size_t)raw;
    buf[len] = '\0';
    /* scoatem newline-ul de la final daca exista */
    if (len > 0 && buf[len - 1] == '\n') {
        buf[len - 1] = '\0';
    }
    return 1;
}

/* parseaza un index pozitiv (1..dep_count) din text */
static int parse_index(const char *text, int *value) {
    if (text == NULL || text[0] == '\0') {
        return 0;
    }
    char *endptr = NULL;
    errno = 0;
    long parsed = strtol(text, &endptr, DECIMAL_BASE);
    if (errno != 0 || *endptr != '\0' || parsed <= 0 || parsed > dep_count) {
        return 0;
    }
    *value = (int)parsed;
    return 1;
}

/* afiseaza meniul principal */
static void show_menu(void) {
    write_str("\n=== Meniu Dependinte ===\n");
    write_str("1. Adauga dependinta\n");
    write_str("2. Modifica dependinta\n");
    write_str("3. Sterge dependinta\n");
    write_str("4. Afiseaza lista\n");
    write_str("5. Genereaza Dockerfile\n");
    write_str("6. Iesire\n");
    write_str("Optiune:> ");
}

/* afiseaza lista numerotata a dependintelor curente */
static void list_deps(void) {
    if (dep_count == 0) {
        write_str("Lista de dependinte este goala.\n");
        return;
    }
    write_str("Dependinte curente:\n");
    for (int i = 0; i < dep_count; i++) {
        write_str("  ");
        write_int(i + 1);
        write_str(". ");
        write_str(dep_list[i]);
        write_str("\n");
    }
}

/* copiaza un string in dep_list la pozitia data */
static void copy_to_dep(int index, const char *src) {
    int pos = 0;
    while (src[pos] != '\0' && pos < DEP_NAME_LEN - 1) {
        dep_list[index][pos] = src[pos];
        pos++;
    }
    dep_list[index][pos] = '\0';
}

/* adauga o dependinta noua in lista */
static void add_dep(void) {
    if (dep_count >= MAX_DEPS) {
        write_str("Eroare: lista este plina (max ");
        write_int(MAX_DEPS);
        write_str(").\n");
        return;
    }
    write_str("Nume pachet: ");
    char name[DEP_NAME_LEN];
    if (!read_input(name, sizeof(name))) {
        write_str("Eroare la citire.\n");
        return;
    }
    if (name[0] == '\0') {
        write_str("Eroare: numele nu poate fi gol.\n");
        return;
    }
    copy_to_dep(dep_count, name);
    dep_count++;
    write_str("Dependinta adaugata.\n");
}

/* modifica o dependinta existenta din lista */
static void modify_dep(void) {
    if (dep_count == 0) {
        write_str("Lista de dependinte este goala.\n");
        return;
    }
    list_deps();
    write_str("Index de modificat: ");
    char input[INPUT_BUF_SIZE];
    int idx = 0;
    if (!read_input(input, sizeof(input)) || !parse_index(input, &idx)) {
        write_str("Index invalid.\n");
        return;
    }
    idx--; /* convertim la 0-based */

    write_str("Nume nou: ");
    char new_name[DEP_NAME_LEN];
    if (!read_input(new_name, sizeof(new_name))) {
        write_str("Eroare la citire.\n");
        return;
    }
    if (new_name[0] == '\0') {
        write_str("Eroare: numele nu poate fi gol.\n");
        return;
    }
    copy_to_dep(idx, new_name);
    write_str("Dependinta modificata.\n");
}

/* sterge o dependinta din lista */
static void delete_dep(void) {
    if (dep_count == 0) {
        write_str("Lista de dependinte este goala.\n");
        return;
    }
    list_deps();
    write_str("Index de sters: ");
    char input[INPUT_BUF_SIZE];
    int idx = 0;
    if (!read_input(input, sizeof(input)) || !parse_index(input, &idx)) {
        write_str("Index invalid.\n");
        return;
    }
    idx--; /* convertim la 0-based */

    /* mutam elementele cu o pozitie in spate */
    for (int i = idx; i < dep_count - 1; i++) {
        copy_to_dep(i, dep_list[i + 1]);
    }
    dep_count--;
    write_str("Dependinta stearsa.\n");
}

/* trimite dependintele la server si primeste Dockerfile-ul generat */
static void generate_dockerfile(int sockfd) {
    if (dep_count == 0) {
        write_str("Eroare: adaugati cel putin o dependinta.\n");
        return;
    }

    /* construim payload-ul: D:dep1 D:dep2 ... */
    char payload[PAYLOAD_BUF_SIZE];
    memset(payload, 0, sizeof(payload));
    size_t plen = 0;

    for (int i = 0; i < dep_count; i++) {
        /* verificam ca mai avem loc in payload */
        size_t dep_len = custom_len(dep_list[i]);
        if (plen + dep_len + DEP_ENTRY_OVERHEAD >= PAYLOAD_BUF_SIZE) {
            break;
        }
        payload[plen++] = 'D';
        payload[plen++] = ':';
        for (size_t j = 0; j < dep_len; j++) {
            payload[plen++] = dep_list[i][j];
        }
        payload[plen++] = ' ';
    }

    write_str("[Client] Trimit ");
    write_int(dep_count);
    write_str(" dependinte la server...\n");
    (void)send(sockfd, payload, plen, 0);

    /* deschidem fisierul de output */
    int out_fd = open("Dockerfile.gen", O_WRONLY | O_CREAT | O_TRUNC, OUTPUT_FILE_MODE);
    if (out_fd < 0) {
        write_str("Eroare la crearea fisierului de output.\n");
        return;
    }

    /* primim Dockerfile-ul de la server pana la marker-ul ===EOF=== */
    char buffer[RECV_BUF_SIZE];
    int eof_found = 0;

    while (!eof_found) {
        ssize_t bytes_received = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
        if (bytes_received <= 0) {
            break;
        }
        buffer[bytes_received] = '\0';

        /* cautam marker-ul de final */
        char *eof_pos = strstr(buffer, "===EOF===");
        size_t write_len = (size_t)bytes_received;
        if (eof_pos != NULL) {
            eof_found = 1;
            write_len = (size_t)(eof_pos - buffer);
        }
        if (write_len > 0) {
            (void)write(out_fd, buffer, write_len);
        }
    }

    (void)close(out_fd);
    write_str("[Client] Dockerfile primit si salvat in Dockerfile.gen\n");
}

int main(void) {
    /* --- 1. SETUP CONEXIUNE TCP --- */
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        write_str("Eroare la creare socket.\n");
        return EXIT_FAILURE;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        write_str("Conexiune la server esuata.\n");
        (void)close(sockfd);
        return EXIT_FAILURE;
    }

    write_str("[Client] Conectat cu succes la server.\n");

    /* --- 2. BUCLA PRINCIPALA - MENIU INTERACTIV --- */
    while (1) {
        show_menu();

        char input[INPUT_BUF_SIZE];
        if (!read_input(input, sizeof(input))) {
            break;
        }

        if (strcmp(input, "1") == 0) {
            add_dep();
        } else if (strcmp(input, "2") == 0) {
            modify_dep();
        } else if (strcmp(input, "3") == 0) {
            delete_dep();
        } else if (strcmp(input, "4") == 0) {
            list_deps();
        } else if (strcmp(input, "5") == 0) {
            generate_dockerfile(sockfd);
        } else if (strcmp(input, "6") == 0 || strcmp(input, "exit") == 0) {
            break;
        } else {
            write_str("Optiune invalida.\n");
        }
    }

    (void)close(sockfd);
    return EXIT_SUCCESS;
}
