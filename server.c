/**
 * P8
 * Proiect PCD - Dockerfile Generator (Server)
 * Aici avem implementarea unui server TCP care primeste comenzi de la un client (dependinte, variabile de mediu, fisiere de copiat) si asambleaza un Dockerfile.
 * Ne folosim de fork() pentru a crea un proces copil pentru fiecare pachet cerut, fiii folosesc doua api uri (repology si fallback -> homebrew) folosind libcurl ca sa verifice existenta dependintei dorite impreuna cu versiunea. la cererea clientului de a pune in dockerfile dependinte variabile de mediu sau fisiere de copy, programul verifica existenata dependintelor, iar daca exista pune sursa si versiunea acesteia, plus celelalte lucruri hardcodate by default clasice unui dockerfile.
 */

#include <stdio.h>       // print, perror
#include <stdlib.h>      // exit()
#include <string.h>      // strcmp, strtok, strstr
#include <unistd.h>      // read, write, close
#include <fcntl.h>       // o_rdonly, o_wronly, o_creat, o_trunc
#include <sys/wait.h>    // waitpid
#include <poll.h>        // poll() pentru multiplexare multi-client
#include <sys/socket.h>  // socket, bind, listen, accept, send, recv
#include <netinet/in.h>  // pentru structurile de ip si porturi
#include <curl/curl.h>   // libraria externa cu care luam json ul de pe internet
#include <libconfig.h>   // libraria cu care citim cfg ul de baza al serverului

#define MAX_CLIENTS 10
#define CURL_BUFFER_SIZE 8192
#define MAX_DEPENDENCIES 10
#define MAX_NAME_LENGTH 64
#define VERSION_LENGTH 64
#define SOURCE_LENGTH 32
#define PATTERN_SIZE 128
#define URL_BUFFER_SIZE 512
#define URL_BASE_LIMIT 480
#define URL_PKG_LIMIT 505
#define URL_HOMEBREW_BASE_LIMIT 470
#define URL_HOMEBREW_PKG_LIMIT 500
#define LOG_BUFFER_SIZE 256
#define ENV_LINE_SIZE 256
#define MESSAGE_BUFFER_SIZE 128
#define VERSION_COMMENT_SIZE 128
#define CLIENT_BUFFER_SIZE 1024
#define PATTERN_KEY_LIMIT 126
#define MAX_ITEM_LENGTH 63
#define DEFAULT_PORT 8080
#define MAX_PORT 65535
#define DECIMAL_BASE 10
#define LISTEN_BACKLOG 5
#define SIN_ZERO_SIZE 8
#define HTTP_OK 200
#define MIN_JSON_RESPONSE 5
#define URL_EXTENSION_BUFFER 6
#define FROM_LABEL_SIZE 5
#define LABEL_MAINTAINER_SIZE 19
#define WORKDIR_PREFIX_SIZE 10
#define COPY_PREFIX_SIZE 5
#define FAILED_PREFIX_SIZE 13
#define FAILED_SUFFIX_SIZE 28
#define CLIENT_CONNECTED_MSG_SIZE 30
#define CLIENT_DISCONNECTED_MSG_SIZE 28
#define TOO_MANY_CLIENTS_MSG_SIZE 51

// Functie utilitara pentru a evita strlen
static size_t custom_len(const char *str) {
    size_t idx = 0;
    while (str[idx] != '\0') {
        idx++;
    }
    return idx;
}

//struct care defineste un buffer dinamic in care se acumuleaza raspunsurile HTTP
//necesar pentru parsarea JSON ului returnat de API
typedef struct {
    char data[CURL_BUFFER_SIZE];
    size_t len;
} CurlBuffer;

//callback pentru libcurl
//copiere date in buffer
static size_t curl_write_buffer(void *ptr, size_t size, size_t nmemb, void *userdata) {
    CurlBuffer *buf = (CurlBuffer *)userdata;
    size_t incoming = size * nmemb;
 
    ///protectie buffer overflow
    if (buf->len + incoming >= sizeof(buf->data) - 1) {
        incoming = sizeof(buf->data) - 1 - buf->len;
    }
 
    int idx;
    for (idx = 0; idx < (int)incoming; idx++) {
        buf->data[buf->len + idx] = ((char *)ptr)[idx];
    }
 
    buf->len += incoming;
    buf->data[buf->len] = '\0';
    return size * nmemb; // returnam size*nmemb, altfel curl semnaleaza eroare
}

static int extract_json_string(const char *json, const char *key, char *out, size_t out_sz) {
    // Construim pattern-ul key : value
    char pattern[PATTERN_SIZE];
    pattern[0] = '"';
    int pattern_idx = 1;
    int key_idx = 0;
    while (key[key_idx] && pattern_idx < PATTERN_KEY_LIMIT) {
        pattern[pattern_idx++] = key[key_idx++];
    }
    pattern[pattern_idx++] = '"';
    pattern[pattern_idx] = '\0';
 
    const char *pos = strstr(json, pattern);
    if (!pos) {
        return 0;
    }
 
    pos += pattern_idx; //sarim peste key
 
    //sarim spatii si ':'
    while (*pos == ' ' || *pos == ':') {
        pos++;
    }
 
    if (*pos != '"') {
        return 0; //valoarea nu e string
    }
    pos++; //sarim ghilimelele de deschidere
 
    size_t out_idx = 0;
    while (*pos && *pos != '"' && out_idx < out_sz - 1) {
        out[out_idx++] = *pos++;
    }
    out[out_idx] = '\0';
 
    return (out_idx > 0) ? 1 : 0;
}

/*
   Strategie dual-API cu fallback:
   
   1. PRIMARY: repology.org/api/v1/project/<pkg>
      - Acoperire nativa Ubuntu/Debian/Alpine (relevante pentru Dockerfile)
      - Returneaza JSON cu toate versiunile disponibile pe toate distro-urile
      - Extragem "newest_version" pentru a o include in Dockerfile 
   
   2. FALLBACK: formulae.brew.sh/api/formula/<pkg>.json
      - Folosit daca repology nu gaseste pachetul
      - Acoperire buna pentru tool-uri de development (cmake, openssl, etc.)
      - Extragem versiunea din campul "versions.stable"
   
   De ce dual-API si nu doar unul?
   - Proiectul genereaza Dockerfiles bazate pe apt-get (Ubuntu/Debian)
   - Homebrew este specific macOS si nu reflecta ce exista in apt
   - repology agrega surse Linux si fallback-ul pe Homebrew prinde restul
*/

//struct rezultat returnata de procesul fiu parintelui prin pipe
//dimensiune fixa = un singur write() atomic (<= PIPE_BUF = 4096 pe Linux
typedef struct {
    int  valid;        // 1 = pachet gasit, 0 = nu exista
    char version[VERSION_LENGTH];  //  versiunea cea mai noua gasita / "" daca nu stim
    char source[SOURCE_LENGTH];   //"repology" / "homebrew" / "not found"
} DepResult;

//verificare via repology
static int check_via_repology(const char *pkg, char *version, size_t vsz) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        return 0;
    }
    // asamblam url ul manual ca sa evitam snprintf, url_idx tine evidenta la ce caracter am ajuns in url, iar pkg_idx parcurge literele pachetului
    char url[URL_BUFFER_SIZE];
    int url_idx = 0;
    const char *base = "https://repology.org/api/v1/project/";
    while (*base && url_idx < URL_BASE_LIMIT) {
        url[url_idx++] = *base++;
    }
    int pkg_idx = 0;
    while (pkg[pkg_idx] && url_idx < URL_PKG_LIMIT) {
        url[url_idx++] = pkg[pkg_idx++];
    }
    url[url_idx] = '\0';
 
    CurlBuffer buf;
    buf.len = 0;
    buf.data[0] = '\0';
 
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_buffer);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "DockerGen/1.0 (demo academic)");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L); // timeout 8s per verificare
 
    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != HTTP_OK) {
        return 0;
    }
    if (buf.len < MIN_JSON_RESPONSE) {
        return 0; // raspuns gol = pachet inexistent
    }
 
    //Repology returneaza un array JSON; daca e "[]" = nu exista
    if (buf.data[0] == '[' && buf.data[1] == ']') {
        return 0;
    }
 
    
    //Extragem "version" din primul obiect din array
    extract_json_string(buf.data, "version", version, vsz);
 
    return 1;
}

//verificare via homebrew
static int check_via_homebrew(const char *pkg, char *version, size_t vsz) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        return 0;
    }
 
    char url[URL_BUFFER_SIZE];
    int url_idx = 0;
    const char *base = "https://formulae.brew.sh/api/formula/";
    while (*base && url_idx < URL_HOMEBREW_BASE_LIMIT) {
        url[url_idx++] = *base++;
    }
    int pkg_idx = 0;
    while (pkg[pkg_idx] && url_idx < URL_HOMEBREW_PKG_LIMIT) {
        url[url_idx++] = pkg[pkg_idx++];
    }
    // adaugam .json
    const char *ext = ".json";
    int ext_idx = 0;
    while (ext[ext_idx]) {
        url[url_idx++] = ext[ext_idx++];
    }
    url[url_idx] = '\0';
 
    CurlBuffer buf;
    buf.len = 0;
    buf.data[0] = '\0';
 
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_buffer);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "DockerGen/1.0 (demo academic)");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
 
    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
 
    if (res != CURLE_OK || http_code != HTTP_OK) {
        return 0;
    }
 
    // Homebrew: versiunea e in "versions":{"stable":"X.Y.Z"}
    // Cautam "stable" in contextul "versions" */
    const char *ver_section = strstr(buf.data, "\"versions\"");
    if (ver_section) {
        extract_json_string(ver_section, "stable", version, vsz);
    }
 
    return 1;
}


//ruleaza in procesul fiu si returneaza DepResult printr un pipe
static void child_check_dep(const char *dep_name, int pipe_fd) {
    DepResult result;
    result.valid = 0;
    result.version[0] = '\0';
    result.source[0] = '\0';
 
    //Mesaj log asamblat atomic (evitam interleaving pe stdout cu alte procese)
    char log[LOG_BUFFER_SIZE];
    int log_idx = 0;
    const char *pre = "[MAP] Verific: ";
    int idx = 0;
    while (pre[idx]) {
        log[log_idx++] = pre[idx++];
    }
    idx = 0;
    while (dep_name[idx]) {
        log[log_idx++] = dep_name[idx++];
    }
 
    //Incearca repology
    if (check_via_repology(dep_name, result.version, sizeof(result.version))) {
        result.valid = 1;
        const char *src = "repology";
        idx = 0;
        while (src[idx]) {
            result.source[idx] = src[idx];
            idx++;
        }
        result.source[idx] = '\0';
 
        const char *msg_ok = " -> OK (repology) v";
        idx = 0;
        while (msg_ok[idx]) {
            log[log_idx] = msg_ok[idx];
            log_idx++;
            idx++;
        }
        idx = 0;
        while (result.version[idx]) {
            log[log_idx] = result.version[idx];
            log_idx++;
            idx++;
        }
        log[log_idx++] = '\n';
        if (write(STDOUT_FILENO, log, (size_t)log_idx) < 0) {
            perror("write");
        }
    }
    //Fallback homebrew
    else if (check_via_homebrew(dep_name, result.version, sizeof(result.version))) {
        result.valid = 1;
        const char *src = "homebrew";
        idx = 0;
        while (src[idx]) {
            result.source[idx] = src[idx];
            idx++;
        }
        result.source[idx] = '\0';
 
        const char *msg_ok = " -> OK (homebrew fallback) v";
        idx = 0;
        while (msg_ok[idx]) {
            log[log_idx] = msg_ok[idx];
            log_idx++;
            idx++;
        }
        idx = 0;
        while (result.version[idx]) {
            log[log_idx] = result.version[idx];
            log_idx++;
            idx++;
        }
        log[log_idx++] = '\n';
        if (write(STDOUT_FILENO, log, (size_t)log_idx) < 0) {
            perror("write");
        }
    }
    //Esec total
    else {
        const char *src = "not found";
        idx = 0;
        while (src[idx]) {
            result.source[idx] = src[idx];
            idx++;
        }
        result.source[idx] = '\0';
 
        const char *err = " -> ESUAT (404 pe ambele API-uri)\n";
        idx = 0;
        while (err[idx]) {
            log[log_idx] = err[idx];
            log_idx++;
            idx++;
        }
        if (write(STDOUT_FILENO, log, (size_t)log_idx) < 0) {
            perror("write");
        }
    }
 
    if (write(pipe_fd, &result, sizeof(DepResult)) < 0) {
        perror("write");
    }
    close(pipe_fd);
}



/* --- LOGICA CENTRALA DE PROCESARE A CERERII --- */
static void process_request_and_send(int client_fd, const char *request) {
    char deps[MAX_DEPENDENCIES][MAX_NAME_LENGTH];
    int dep_count = 0;
    char envs[MAX_DEPENDENCIES][MAX_NAME_LENGTH];
    int env_count = 0;
    char copies[MAX_DEPENDENCIES][MAX_NAME_LENGTH];
    int copy_count = 0;

    // Resetare matrici manual
    for (int idx = 0; idx < MAX_DEPENDENCIES; idx++) {
        for (int jdx = 0; jdx < MAX_NAME_LENGTH; jdx++) {
            deps[idx][jdx] = '\0';
            envs[idx][jdx] = '\0';
            copies[idx][jdx] = '\0';
        }
    }

    // PASUL 2A: PARSAREA
    size_t req_idx = 0;
    while (request[req_idx] != '\0') {
        while (request[req_idx] == ' ') {
            req_idx++;
        }
        if (request[req_idx] == '\0') {
            break;
        }

        char type = request[req_idx];
        if (request[req_idx + 1] == ':') {
            req_idx += 2;
            size_t char_idx = 0;
            
            while (request[req_idx] != ' ' && request[req_idx] != '\0' && char_idx < MAX_ITEM_LENGTH) {
                if (type == 'D' && dep_count < MAX_DEPENDENCIES) {
                    deps[dep_count][char_idx++] = request[req_idx];
                } else if (type == 'E' && env_count < MAX_DEPENDENCIES) {
                    envs[env_count][char_idx++] = request[req_idx];
                } else if (type == 'C' && copy_count < MAX_DEPENDENCIES) {
                    if (request[req_idx] == ',') {
                        copies[copy_count][char_idx++] = ' ';
                    } else {
                        copies[copy_count][char_idx++] = request[req_idx];
                    }
                }
                req_idx++;
            }
            if (type == 'D' && dep_count < MAX_DEPENDENCIES) {
                dep_count++;
            }
            if (type == 'E' && env_count < MAX_DEPENDENCIES) {
                env_count++;
            }
            if (type == 'C' && copy_count < MAX_DEPENDENCIES) {
                copy_count++;
            }
        } else {
            while (request[req_idx] != ' ' && request[req_idx] != '\0') {
                req_idx++;
            }
        }
    }

    // PASUL 2B: MAP-REDUCE (Paralelizare)
    pid_t pids[MAX_DEPENDENCIES];
    int pipe_fds[MAX_DEPENDENCIES][2]; //[i][0]=citire(parinte), [i][1]=scriere(fiu)
    DepResult results[MAX_DEPENDENCIES];

    for (int idx = 0; idx < MAX_DEPENDENCIES; idx++) {
        results[idx].valid      = 0;
        results[idx].version[0] = '\0';
        results[idx].source[0]  = '\0';
        pipe_fds[idx][0] = pipe_fds[idx][1] = -1;
    }

    // MAP: cream cate un fiu per dependinta
    for (int idx = 0; idx < dep_count; idx++) {
        if (pipe(pipe_fds[idx]) < 0) {
            pids[idx] = -1;
            continue;
        }
 
        pids[idx] = fork();
        if (pids[idx] == 0) {
            //fiul
            close(pipe_fds[idx][0]); //fiul nu citeste din pipe
            child_check_dep(deps[idx], pipe_fds[idx][1]);
            _Exit(0); //exit code nu mai conteaza, datele sunt in pipe
        } else {
            //parintele
            close(pipe_fds[idx][1]); //parintele nu scrie in pipe
        }
    }


    // REDUCE: Parintele asteapta copiii
    for (int idx = 0; idx < dep_count; idx++) {
        if (pids[idx] > 0) {
            int status;
            if (waitpid(pids[idx], &status, 0) < 0) {
                perror("waitpid");
            }
            ssize_t bytes_read = read(pipe_fds[idx][0], &results[idx], sizeof(DepResult));
            if (bytes_read != sizeof(DepResult)) {
                results[idx].valid = 0;
            }
            close(pipe_fds[idx][0]);
        }
    }

    // PASUL 2C: CITIRE CONFIGURATIE (libconfig)
    config_t cfg;
    config_init(&cfg);

    const char *base_image = "ubuntu:latest";
    const char *maintainer = "necunoscut";
    const char *workdir = "/tmp";

    if (config_read_file(&cfg, "demo.cfg")) {
        config_lookup_string(&cfg, "container.base_image", &base_image);
        config_lookup_string(&cfg, "container.maintainer", &maintainer);
        config_lookup_string(&cfg, "container.workdir", &workdir);
    }

    send(client_fd, "FROM ", FROM_LABEL_SIZE, 0);
    send(client_fd, base_image, custom_len(base_image), 0);
    send(client_fd, "\nLABEL maintainer=\"", LABEL_MAINTAINER_SIZE, 0);
    send(client_fd, maintainer, custom_len(maintainer), 0);
    send(client_fd, "\"\nWORKDIR ", WORKDIR_PREFIX_SIZE, 0);
    send(client_fd, workdir, custom_len(workdir), 0);
    send(client_fd, "\n\n", 2, 0);

    for (int idx = 0; idx < env_count; idx++) {
        send(client_fd, "ENV ", 4, 0);
        send(client_fd, envs[idx], custom_len(envs[idx]), 0);
        send(client_fd, "\n", 1, 0);
    }
    if (env_count > 0) {
        send(client_fd, "\n", 1, 0);
    }

    for (int idx = 0; idx < copy_count; idx++) {
        send(client_fd, "COPY ", COPY_PREFIX_SIZE, 0);
        send(client_fd, copies[idx], custom_len(copies[idx]), 0);
        send(client_fd, "\n", 1, 0);
    }
    if (copy_count > 0) {
        send(client_fd, "\n", 1, 0);
    }

    if (dep_count > 0) {
        const char *run_start = "RUN apt-get update && apt-get install -y \\\n";
        send(client_fd, run_start, custom_len(run_start), 0);
        
        for (int idx = 0; idx < dep_count; idx++) {
            if (results[idx].valid) {
                send(client_fd, "    ", 4, 0);
                send(client_fd, deps[idx], custom_len(deps[idx]), 0);
                if (results[idx].version[0] != '\0') {
                    char ver_comment[VERSION_COMMENT_SIZE];
                    // construim " \  # v<version> (<source>)\n"
                    int ver_idx = 0;
                    const char *prefix = " \\  # v";
                    int kdx = 0;
                    while (prefix[kdx]) {
                        ver_comment[ver_idx++] = prefix[kdx++];
                    }
                    kdx = 0;
                    while (results[idx].version[kdx]) {
                        ver_comment[ver_idx++] = results[idx].version[kdx++];
                    }
                    ver_comment[ver_idx++] = ' ';
                    ver_comment[ver_idx++] = '(';
                    kdx = 0;
                    while (results[idx].source[kdx]) {
                        ver_comment[ver_idx++] = results[idx].source[kdx++];
                    }
                    ver_comment[ver_idx++] = ')';
                    ver_comment[ver_idx++] = '\n';
                    ver_comment[ver_idx] = '\0';
                    send(client_fd, ver_comment, (size_t)ver_idx, 0);
                } else {
                    send(client_fd, " \\\n", 3, 0);
                }
            } else {
                send(client_fd, "    # ESUAT: ", FAILED_PREFIX_SIZE, 0);
                send(client_fd, deps[idx], custom_len(deps[idx]), 0);
                send(client_fd, " (Lipseste din repozitoriu)\n", FAILED_SUFFIX_SIZE, 0);
            }
        }
        const char *run_end = "    && apt-get clean \\\n    && rm -rf /var/lib/apt/lists/*\n\n";
        send(client_fd, run_end, custom_len(run_end), 0);
    }

    const char *footer = "CMD [\"/bin/bash\"]\n";
    send(client_fd, footer, custom_len(footer), 0);

    const char *eof_marker = "\n===EOF===\n";
    send(client_fd, eof_marker, custom_len(eof_marker), 0);

    config_destroy(&cfg);
}

static void load_env_file(const char *filename) {
    FILE *file_ptr = fopen(filename, "r");
    if (file_ptr == NULL) {
        return; // daca nu exista .env, mergem pe valorile default din cod
    }

    char line[ENV_LINE_SIZE];
    while (fgets(line, sizeof(line), file_ptr) != NULL) {
        if (line[0] == '\n' || line[0] == '#' || line[0] == '\r') {
            continue;
        }

        char *separator = strchr(line, '=');
        if (separator != NULL) {
            *separator = '\0';
            char *key = line;
            char *value = separator + 1;

            size_t len = custom_len(value);
            if (len > 0 && value[len - 1] == '\n') {
                value[len - 1] = '\0';
            }
            len = custom_len(value);
            if (len > 0 && value[len - 1] == '\r') {
                value[len - 1] = '\0';
            }

            if (setenv(key, value, 1) != 0) {
                perror("setenv");
            }
        }
    }
    if (fclose(file_ptr) != 0) {
        perror("fclose");
    }
}

/* --- 3. SERVERUL PRINCIPAL --- */
int main(void) {
    load_env_file(".env");

    int port = DEFAULT_PORT; // portul default (fallback)
    char *env_port = getenv("SERVER_PORT");
    if (env_port != NULL) {
        char *endptr;
        long val = strtol(env_port, &endptr, DECIMAL_BASE);
        if (*endptr == '\0' && val > 0 && val <= MAX_PORT) {
            port = (int)val;
        }
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Eroare socket");
        return 1;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
    }

    struct sockaddr_in address;
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port        = htons((uint16_t)port);
    for (int idx = 0; idx < SIN_ZERO_SIZE; idx++) {
        address.sin_zero[idx] = '\0';
    }

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Eroare la bind");
        return 1;
    }

    if (listen(server_fd, LISTEN_BACKLOG) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // construim mesajul de pornire fara snprintf (evitam warning-ul de securitate)
    char msg_start[64];
    int mi = 0;
    const char *ms1 = "[Server] Ascult pe port ";
    for (int k = 0; ms1[k]; k++) msg_start[mi++] = ms1[k];
    int tmp[10]; int ti = 0; int p = port;
    if (p == 0) { msg_start[mi++] = '0'; }
    else { while (p > 0) { tmp[ti++] = p % 10; p /= 10; } for (int k = ti - 1; k >= 0; k--) msg_start[mi++] = (char)('0' + tmp[k]); }
    const char *ms2 = "...\n";
    for (int k = 0; ms2[k]; k++) msg_start[mi++] = ms2[k];
    if (write(STDOUT_FILENO, msg_start, (size_t)mi) < 0) perror("Eroare la pornire");

    // MULTIPLEXARE cu poll()
    struct pollfd fds[MAX_CLIENTS];
    for (int idx = 0; idx < MAX_CLIENTS; idx++) {
        fds[idx].fd = -1;
    }
    fds[0].fd     = server_fd;
    fds[0].events = POLLIN;


    while (1) {
        int ready = poll(fds, MAX_CLIENTS, -1);
        if (ready < 0) {
            break;
        }

        // Conexiune noua
        if (fds[0].revents & POLLIN) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int new_socket = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
            
            if (new_socket >= 0) {
                if (write(STDOUT_FILENO, "[Server] Client nou conectat.\n", CLIENT_CONNECTED_MSG_SIZE) < 0) {
                    perror("write");
                }
                int slot_found = 0;
                for (int idx = 1; idx < MAX_CLIENTS; idx++) {
                    if (fds[idx].fd < 0) {
                        fds[idx].fd = new_socket;
                        fds[idx].events = POLLIN;
                        slot_found = 1;
                        break;
                    }
                }
                if (slot_found == 0) {
                    if (write(STDOUT_FILENO, "[Server] Prea multi clienti, conexiune refuzata.\n", TOO_MANY_CLIENTS_MSG_SIZE) < 0) {
                        perror("write");
                    }
                    close(new_socket);
                }
            }
        }

        // Citire date de la clientii conectati
        for (int idx = 1; idx < MAX_CLIENTS; idx++) {
            if (fds[idx].fd > 0 && (fds[idx].revents & POLLIN)) {
                char buffer[CLIENT_BUFFER_SIZE];
                for (int jdx = 0; jdx < CLIENT_BUFFER_SIZE; jdx++) {
                    buffer[jdx] = '\0';
                }

                ssize_t bytes_read = recv(fds[idx].fd, buffer, sizeof(buffer) - 1, 0);
                
                if (bytes_read <= 0) {
                    if (write(STDOUT_FILENO, "[Server] Client deconectat.\n", CLIENT_DISCONNECTED_MSG_SIZE) < 0) {
                        perror("write");
                    }
                    close(fds[idx].fd);
                    fds[idx].fd = -1;
                } else {
                    process_request_and_send(fds[idx].fd, buffer);
                }
            }
        }
    }

    close(server_fd);
    curl_global_cleanup();
    return 0;
}