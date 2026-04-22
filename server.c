/**
 * Echipa 8
 * IR3 2026
 * Proiect PCD - Server
 * Rezumat: server TCP cu multiplexare (poll) care primeste liste de dependinte
 * de la clienti, le verifica online prin dual-API (repology + homebrew fallback)
 * folosind procese fiu (fork + pipe), citeste configuratia din demo.cfg
 * cu libconfig si genereaza un Dockerfile pe care il trimite inapoi clientului.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <curl/curl.h>
#include <libconfig.h>
#include <string.h>

#define MAX_CLIENTS 10
#define PORT 8080
#define CURL_BUFFER_SIZE 8192

// Functie utilitara pentru a evita strlen
size_t custom_len(const char *str) {
    size_t i = 0;
    while (str[i] != '\0') i++;
    return i;
}

//struct care defineste un buffer dinamic in care se acumuleaza raspunsurile HTTP
//necesar pentru parsarea JSON ului returnat de API
typedef struct {
    char data[CURL_BUFFER_SIZE];
    size_t len;
} CurlBuffer;

//nu se poate extrage versiunea fara sa accesam JSON ul
// Functie dummy pentru a impiedica libcurl sa afiseze JSON-ul descarcat pe ecran
size_t curl_dummy_write(void *ptr, size_t size, size_t nmemb, void *userdata) {
    return size * nmemb; 
}

//callback pentur libcurl
//copiere date in buffer
size_t curl_write_buffer(void *ptr, size_t size, size_t nmemb, void *userdata) {
    CurlBuffer *buf = (CurlBuffer *)userdata;
    size_t incoming = size * nmemb;
 
    ///protectie buffer overflow 
    if (buf->len + incoming >= sizeof(buf->data) - 1)
        incoming = sizeof(buf->data) - 1 - buf->len;
 
    int i;
    for (i = 0; i < (int)incoming; i++)
        buf->data[buf->len + i] = ((char *)ptr)[i];
 
    buf->len += incoming;
    buf->data[buf->len] = '\0';
    return size * nmemb; // returnam size*nmemb, altfel curl semnaleaza eroare 
}

int extract_json_string(const char *json, const char *key, char *out, size_t out_sz) {
    // Construim pattern-ul key : value
    char pattern[128];
    pattern[0] = '"';
    int pi = 1;
    int ki = 0;
    while (key[ki] && pi < 126) pattern[pi++] = key[ki++];
    pattern[pi++] = '"';
    pattern[pi] = '\0';
 
    const char *pos = strstr(json, pattern);
    if (!pos) return 0;
 
    pos += pi; //sarim peste key
 
    //sarim spatii si ':' 
    while (*pos == ' ' || *pos == ':') pos++;
 
    if (*pos != '"') return 0; //valoarea nu e string 
    pos++; //sarim ghilimelele de deschidere 
 
    size_t i = 0;
    while (*pos && *pos != '"' && i < out_sz - 1)
        out[i++] = *pos++;
    out[i] = '\0';
 
    return (i > 0) ? 1 : 0;
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
    char version[64];  //  versiunea cea mai noua gasita / "" daca nu stim 
    char source[32];   //"repology" / "homebrew" / "not found" 
} DepResult;

//verificare via repology
static int check_via_repology(const char *pkg, char *version, size_t vsz) {
    CURL *curl = curl_easy_init();
    if (!curl) return 0;
 
    // Construim URL manual: https://repology.org/api/v1/project/<pkg> 
    char url[512];
    int ui = 0;
    const char *base = "https://repology.org/api/v1/project/";
    while (*base && ui < 480) url[ui++] = *base++;
    int pi = 0;
    while (pkg[pi] && ui < 505) url[ui++] = pkg[pi++];
    url[ui] = '\0';
 
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

    char debug_msg[256];
    int di = 0;
    const char *dp = "[DEBUG repology] ";
    while(dp[di]) debug_msg[di] = dp[di++];
    int bi = 0;
    while(buf.data[bi] && bi < 200) debug_msg[di++] = buf.data[bi++];
    debug_msg[di++] = '\n';
    write(STDOUT_FILENO, debug_msg, di);
 
    if (res != CURLE_OK || http_code != 200) return 0;
    if (buf.len < 5) return 0; // raspuns gol = pachet inexistent 
 
    //Repology returneaza un array JSON; daca e "[]" = nu exista 
    if (buf.data[0] == '[' && buf.data[1] == ']') return 0;
 
    
    //Extragem "newest_version" din primul obiect din array 
    extract_json_string(buf.data, "version", version, vsz);
 
    return 1;
}

//verificare via homebrew
static int check_via_homebrew(const char *pkg, char *version, size_t vsz) {
    CURL *curl = curl_easy_init();
    if (!curl) return 0;
 
    char url[512];
    int ui = 0;
    const char *base = "https://formulae.brew.sh/api/formula/";
    while (*base && ui < 470) url[ui++] = *base++;
    int pi = 0;
    while (pkg[pi] && ui < 500) url[ui++] = pkg[pi++];
    // adaugam .json 
    const char *ext = ".json";
    int ei = 0;
    while (ext[ei]) url[ui++] = ext[ei++];
    url[ui] = '\0';
 
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
 
    if (res != CURLE_OK || http_code != 200) return 0;
 
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
    char log[256];
    int li = 0;
    const char *pre = "[MAP] Verific: ";
    int i = 0;
    while (pre[i]) log[li++] = pre[i++];
    i = 0;
    while (dep_name[i]) log[li++] = dep_name[i++];
 
    //Incearca repology
    if (check_via_repology(dep_name, result.version, sizeof(result.version))) {
        result.valid = 1;
        const char *src = "repology";
        i = 0;
        while (src[i]) result.source[i] = src[i++];
        result.source[i] = '\0';
 
        const char *ok = " -> OK (repology) v";
        i = 0; while (ok[i]) log[li++] = ok[i++];
        i = 0; while (result.version[i]) log[li++] = result.version[i++];
        log[li++] = '\n';
        write(STDOUT_FILENO, log, li);
    }
    //Fallback homebrew
    else if (check_via_homebrew(dep_name, result.version, sizeof(result.version))) {
        result.valid = 1;
        const char *src = "homebrew";
        i = 0;
        while (src[i]) result.source[i] = src[i++];
        result.source[i] = '\0';
 
        const char *ok = " -> OK (homebrew fallback) v";
        i = 0; while (ok[i]) log[li++] = ok[i++];
        i = 0; while (result.version[i]) log[li++] = result.version[i++];
        log[li++] = '\n';
        write(STDOUT_FILENO, log, li);
    }
    //Esec total
    else {
        const char *src = "not found";
        i = 0;
        while (src[i]) result.source[i] = src[i++];
        result.source[i] = '\0';
 
        const char *err = " -> ESUAT (404 pe ambele API-uri)\n";
        i = 0; while (err[i]) log[li++] = err[i++];
        write(STDOUT_FILENO, log, li);
    }
 
    // Trimitem struct-ul rezultat parintelui prin pipe 
    write(pipe_fd, &result, sizeof(DepResult));
    close(pipe_fd);
}



/* --- 1. VERIFICAREA PACHETELOR (MAP) --- */
int check_dependency_online(const char *dep_name) {
    CURL *curl;
    CURLcode res;
    long response_code = 0;
    int success = 0;

    char url[512];
    for (int k = 0; k < 512; k++) url[k] = '\0'; 

    // Folosim API-ul Homebrew: este ultra-rapid si returneaza 404 real daca pachetul nu exista
    const char *base = "https://formulae.brew.sh/api/formula/";
    size_t i = 0;
    while (base[i] != '\0' && i < sizeof(url) - 6) {
        url[i] = base[i];
        i++;
    }
    size_t j = 0;
    while (dep_name[j] != '\0' && i < sizeof(url) - 6) {
        url[i++] = dep_name[j++];
    }
    
    // Adaugam extensia .json
    char *ext = ".json";
    int e = 0;
    while(ext[e] != '\0') url[i++] = ext[e++];

    curl = curl_easy_init();
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_dummy_write); // Ascundem output-ul
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "DockerGen/1.0");

        res = curl_easy_perform(curl);
        if(res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
            if (response_code == 200) success = 1; // Pachet valid!
        }
        curl_easy_cleanup(curl);
    }
    return success; 
}

/* --- 2. LOGICA CENTRALA DE PROCESARE A CERERII --- */
void process_request_and_send(int client_fd, char *request) {
    char deps[10][64];  int dep_count = 0;
    char envs[10][64];  int env_count = 0;
    char copies[10][64]; int copy_count = 0;

    // Resetare matrici manual
    for (int i = 0; i < 10; i++) {
        for (int j = 0; j < 64; j++) {
            deps[i][j] = '\0'; envs[i][j] = '\0'; copies[i][j] = '\0';
        }
    }

    // PASUL 2A: PARSAREA
    size_t req_idx = 0;
    while (request[req_idx] != '\0') {
        while (request[req_idx] == ' ') req_idx++; 
        if (request[req_idx] == '\0') break;

        char type = request[req_idx]; 
        if (request[req_idx + 1] == ':') {
            req_idx += 2; 
            size_t char_idx = 0;
            
            while (request[req_idx] != ' ' && request[req_idx] != '\0' && char_idx < 63) {
                if (type == 'D' && dep_count < 10) deps[dep_count][char_idx++] = request[req_idx];
                else if (type == 'E' && env_count < 10) envs[env_count][char_idx++] = request[req_idx];
                else if (type == 'C' && copy_count < 10) {
                    if (request[req_idx] == ',') copies[copy_count][char_idx++] = ' '; 
                    else copies[copy_count][char_idx++] = request[req_idx];
                }
                req_idx++;
            }
            if (type == 'D' && dep_count < 10) dep_count++;
            if (type == 'E' && env_count < 10) env_count++;
            if (type == 'C' && copy_count < 10) copy_count++;
        } else {
            while (request[req_idx] != ' ' && request[req_idx] != '\0') req_idx++;
        }
    }

    // PASUL 2B: MAP-REDUCE (Paralelizare)
    pid_t pids[10];
    int valid_deps[10] = {0};
    int pipe_fds[10][2]; //[i][0]=citire(parinte), [i][1]=scriere(fiu)
    DepResult results[10];

    for (int i = 0; i < 10; i++) {
        results[i].valid      = 0;
        results[i].version[0] = '\0';
        results[i].source[0]  = '\0';
        pipe_fds[i][0] = pipe_fds[i][1] = -1;
    }

    // MAP: Fiii fac verificarea online
    /*for (int i = 0; i < dep_count; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            // Asamblam mesajul complet in memorie pentru un singur apel write()
            // Acest lucru previne "Race Conditions" pe consola (ex: litere amestecate)
            char msg_buf[256];
            for (int k = 0; k < 256; k++) msg_buf[k] = '\0';
            
            char *prefix = "[MAP] Verific pachet: ";
            size_t idx = 0;
            while(prefix[idx]) { msg_buf[idx] = prefix[idx]; idx++; }
            
            size_t d_idx = 0;
            while(deps[i][d_idx]) { msg_buf[idx++] = deps[i][d_idx++]; }
            
            int is_valid = check_dependency_online(deps[i]); 
            
            if(is_valid) {
                char *res_ok = " -> EXISTĂ (200 OK)\n";
                int r = 0; while(res_ok[r]) { msg_buf[idx++] = res_ok[r++]; }
                write(STDOUT_FILENO, msg_buf, idx);
                exit(0); 
            } else {
                char *res_err = " -> EȘUAT (404 Not Found)\n";
                int r = 0; while(res_err[r]) { msg_buf[idx++] = res_err[r++]; }
                write(STDOUT_FILENO, msg_buf, idx);
                exit(1); 
            }
        }
    }*/
    // MAP: cream cate un fiu per dependinta 
    for (int i = 0; i < dep_count; i++) {
        if (pipe(pipe_fds[i]) < 0) { pids[i] = -1; continue; }
 
        pids[i] = fork();
        if (pids[i] == 0) {
            //fiul
            close(pipe_fds[i][0]); //fiul nu citeste din pipe 
            child_check_dep(deps[i], pipe_fds[i][1]);
            exit(0); //exit code nu mai conteaza, datele sunt in pipe 
        } else {
            //parintele
            close(pipe_fds[i][1]); //parintele nu scrie in pipe 
        }
    }


    // REDUCE: Parintele asteapta copiii
    for (int i = 0; i < dep_count; i++) {
        //mut declaratia asta in if int status;
        if (pids[i] > 0) {
            int status;
            waitpid(pids[i], &status, 0);
            /*if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                valid_deps[i] = 1; 
            }*/
            ssize_t rd = read(pipe_fds[i][0], &results[i], sizeof(DepResult));
            if (rd != sizeof(DepResult)) results[i].valid = 0; 
            close(pipe_fds[i][0]);
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

    // PASUL 2D: ASAMBLAREA DOCKERFILE-ULUI
    send(client_fd, "FROM ", 5, 0);
    send(client_fd, base_image, custom_len(base_image), 0);
    send(client_fd, "\nLABEL maintainer=\"", 19, 0);
    send(client_fd, maintainer, custom_len(maintainer), 0);
    send(client_fd, "\"\nWORKDIR ", 10, 0);
    send(client_fd, workdir, custom_len(workdir), 0);
    send(client_fd, "\n\n", 2, 0);

    for (int i = 0; i < env_count; i++) {
        send(client_fd, "ENV ", 4, 0);
        send(client_fd, envs[i], custom_len(envs[i]), 0);
        send(client_fd, "\n", 1, 0);
    }
    if (env_count > 0) send(client_fd, "\n", 1, 0);

    for (int i = 0; i < copy_count; i++) {
        send(client_fd, "COPY ", 5, 0);
        send(client_fd, copies[i], custom_len(copies[i]), 0);
        send(client_fd, "\n", 1, 0);
    }
    if (copy_count > 0) send(client_fd, "\n", 1, 0);

    if (dep_count > 0) {
        char *run_start = "RUN apt-get update && apt-get install -y \\\n";
        send(client_fd, run_start, custom_len(run_start), 0);
        
        for (int i = 0; i < dep_count; i++) {
            if (results[i].valid) { 
                send(client_fd, "    ", 4, 0);
                send(client_fd, deps[i], custom_len(deps[i]), 0);
                if (results[i].version[0] != '\0') {
                    char ver_comment[128];
                    // construim " \  # v<version> (<source>)\n" 
                    int vi = 0;
                    const char *p1 = " \\  # v";
                    int k = 0; while (p1[k]) ver_comment[vi++] = p1[k++];
                    k = 0; while (results[i].version[k]) ver_comment[vi++] = results[i].version[k++];
                    ver_comment[vi++] = ' '; ver_comment[vi++] = '(';
                    k = 0; while (results[i].source[k]) ver_comment[vi++] = results[i].source[k++];
                    ver_comment[vi++] = ')'; ver_comment[vi++] = '\n';
                    ver_comment[vi] = '\0';
                    send(client_fd, ver_comment, vi, 0);
                } else {
                    send(client_fd, " \\\n", 3, 0);
                }
            } else {
                send(client_fd, "    # ESUAT: ", 13, 0);
                send(client_fd, deps[i], custom_len(deps[i]), 0);
                send(client_fd, " (Lipseste din repozitoriu)\n", 28, 0);
            }
        }
        char *run_end = "    && apt-get clean \\\n    && rm -rf /var/lib/apt/lists/*\n\n";
        send(client_fd, run_end, custom_len(run_end), 0);
    }

    char *footer = "CMD [\"/bin/bash\"]\n";
    send(client_fd, footer, custom_len(footer), 0);

    // Marker-ul pentru a spune clientului ca s-a terminat fisierul
    char *eof_marker = "\n===EOF===\n";
    send(client_fd, eof_marker, custom_len(eof_marker), 0);

    config_destroy(&cfg);
}

/* --- 3. SERVERUL PRINCIPAL --- */
int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Eroare socket"); return 1; 
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); 

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; 
    address.sin_port = htons(8080);
    for (int i = 0; i < 8; i++) address.sin_zero[i] = '\0';

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Eroare la bind");
        return 1;
    }

    listen(server_fd, 5); 
    curl_global_init(CURL_GLOBAL_DEFAULT); 
    
    char msg_start[] = "[Server] Ascult pe port 8080...\n";
    write(STDOUT_FILENO, msg_start, custom_len(msg_start));

    // MULTIPLEXARE
    struct pollfd fds[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++) fds[i].fd = -1; 
    
    fds[0].fd = server_fd; 
    fds[0].events = POLLIN;

    while (1) {
        int ready = poll(fds, MAX_CLIENTS, -1); 
        if (ready < 0) break;

        // Conexiune noua
        if (fds[0].revents & POLLIN) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int new_socket = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
            
            if (new_socket >= 0) {
                write(STDOUT_FILENO, "[Server] Client nou conectat.\n", 30);
                for (int i = 1; i < MAX_CLIENTS; i++) {
                    if (fds[i].fd < 0) {
                        fds[i].fd = new_socket;
                        fds[i].events = POLLIN;
                        break;
                    }
                }
            }
        }

        // Citire date de la clientii conectati
        for (int i = 1; i < MAX_CLIENTS; i++) {
            if (fds[i].fd > 0 && (fds[i].revents & POLLIN)) {
                char buffer[1024];
                for (int j = 0; j < 1024; j++) buffer[j] = '\0';

                ssize_t bytes_read = recv(fds[i].fd, buffer, sizeof(buffer) - 1, 0);
                
                if (bytes_read <= 0) {
                    write(STDOUT_FILENO, "[Server] Client deconectat.\n", 28);
                    close(fds[i].fd);
                    fds[i].fd = -1; 
                } else {
                    process_request_and_send(fds[i].fd, buffer);
                }
            }
        }
    }

    close(server_fd);
    curl_global_cleanup();
    return 0;
}