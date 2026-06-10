


/**
 * P8
 * Proiect PCD - Dockerfile Generator (Server)
 * Server TCP care primeste comenzi de la clienti (dependinte, variabile de mediu, fisiere)
 * si asambleaza un Dockerfile.
 *
 * Nivel B:
 *   - poll() pentru I/O multiplexing (FCE Clienti Ordinari)
 *   - pipe anonim pentru coada FIFO intre I/O thread si processing thread
 *   - pthread exclusiv pentru sincronizare (mutex)
 *   - inotify pentru monitorizarea directorului uploads/
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <dirent.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <curl/curl.h>
#include <libconfig.h>
#include <stdarg.h>
#include <signal.h>
#include <errno.h>

#define CURL_BUFFER_SIZE 8192
#define ADMIN_PORT       8081
#define MAX_CLIENTS      64
#define WORK_DATA_SIZE   1024
#define LOG_FILE         "server.log"

// --- Sistem de logging thread-safe (stdout + fisier cu timestamp) ---

typedef enum { LOG_INFO, LOG_WARN, LOG_ERROR } LogLevel;

static FILE           *g_log_file  = NULL;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static void server_log(LogLevel level, const char *fmt, ...) {
    time_t     now     = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[24];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    const char *lvl = (level == LOG_INFO)  ? "INFO " :
                      (level == LOG_WARN)  ? "WARN " : "ERROR";

    char body[480];
    va_list args;
    va_start(args, fmt);
    vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);

    char line[560];
    int n = snprintf(line, sizeof(line), "[%s] [%s] %s\n", timestamp, lvl, body);
    if (n <= 0 || n >= (int)sizeof(line)) n = sizeof(line) - 1;

    pthread_mutex_lock(&g_log_mutex);
    write(STDOUT_FILENO, line, (size_t)n);
    if (g_log_file) {
        fwrite(line, 1, (size_t)n, g_log_file);
        fflush(g_log_file);
    }
    pthread_mutex_unlock(&g_log_mutex);
}

// pipe anonim: I/O thread scrie WorkItem, processing thread citeste (Nivel B: pipe anonim)
static int g_work_pipe[2] = {-1, -1};

// WorkItem transportat prin pipe: dimensiunea < PIPE_BUF (4096) => scriere atomica
typedef struct {
    int     client_fd;
    char    data[WORK_DATA_SIZE];
    ssize_t data_len;
} WorkItem;

// registrul global al clientilor TCP conectati
typedef struct {
    int  fd;
    char ip[INET_ADDRSTRLEN];
} ClientInfo;

static ClientInfo      g_clients[MAX_CLIENTS];
static int             g_client_count = 0;
static pthread_mutex_t g_clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static int             g_tcp_port   = 8080;
static time_t          g_start_time = 0;
volatile sig_atomic_t  g_running    = 1; // setat pe 0 de SIGINT/SIGTERM sau comanda shutdown

static void sigint_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static void client_register(int fd, const char *ip) {
    pthread_mutex_lock(&g_clients_mutex);
    if (g_client_count < MAX_CLIENTS) {
        g_clients[g_client_count].fd = fd;
        int i = 0;
        while (ip[i] && i < INET_ADDRSTRLEN - 1) { g_clients[g_client_count].ip[i] = ip[i]; i++; }
        g_clients[g_client_count].ip[i] = '\0';
        g_client_count++;
    }
    pthread_mutex_unlock(&g_clients_mutex);
}

static void client_unregister(int fd) {
    pthread_mutex_lock(&g_clients_mutex);
    for (int i = 0; i < g_client_count; i++) {
        if (g_clients[i].fd == fd) {
            for (int j = i; j < g_client_count - 1; j++) g_clients[j] = g_clients[j + 1];
            g_client_count--;
            break;
        }
    }
    pthread_mutex_unlock(&g_clients_mutex);
}

size_t custom_len(const char *str) {
    size_t i = 0;
    while (str[i] != '\0') i++;
    return i;
}

typedef struct {
    char   data[CURL_BUFFER_SIZE];
    size_t len;
} CurlBuffer;

size_t curl_dummy_write(void *ptr, size_t size, size_t nmemb, void *userdata) {
    (void)ptr; (void)userdata;
    return size * nmemb;
}

size_t curl_write_buffer(void *ptr, size_t size, size_t nmemb, void *userdata) {
    CurlBuffer *buf = (CurlBuffer *)userdata;
    size_t incoming = size * nmemb;

    if (buf->len + incoming >= sizeof(buf->data) - 1)
        incoming = sizeof(buf->data) - 1 - buf->len;

    for (int i = 0; i < (int)incoming; i++)
        buf->data[buf->len + i] = ((char *)ptr)[i];

    buf->len += incoming;
    buf->data[buf->len] = '\0';
    return size * nmemb;
}

int extract_json_string(const char *json, const char *key, char *out, size_t out_sz) {
    char pattern[128];
    pattern[0] = '"';
    int pi = 1, ki = 0;
    while (key[ki] && pi < 126) pattern[pi++] = key[ki++];
    pattern[pi++] = '"';
    pattern[pi] = '\0';

    const char *pos = strstr(json, pattern);
    if (!pos) return 0;

    pos += pi;
    while (*pos == ' ' || *pos == ':') pos++;
    if (*pos != '"') return 0;
    pos++;

    size_t i = 0;
    while (*pos && *pos != '"' && i < out_sz - 1) out[i++] = *pos++;
    out[i] = '\0';

    return (i > 0) ? 1 : 0;
}

typedef struct {
    char dep[64];
    int  client_fd;
} DepArgs;

static int check_via_repology(const char *pkg, char *version, size_t vsz) {
    CURL *curl = curl_easy_init();
    if (!curl) return 0;

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
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "DockerGen/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) return 0;
    if (buf.len < 5) return 0;
    if (buf.data[0] == '[' && buf.data[1] == ']') return 0;

    extract_json_string(buf.data, "version", version, vsz);
    return 1;
}

static int check_via_homebrew(const char *pkg, char *version, size_t vsz) {
    CURL *curl = curl_easy_init();
    if (!curl) return 0;

    char url[512];
    int ui = 0;
    const char *base = "https://formulae.brew.sh/api/formula/";
    while (*base && ui < 470) url[ui++] = *base++;

    int pi = 0;
    while (pkg[pi] && ui < 500) url[ui++] = pkg[pi++];

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
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "DockerGen/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) return 0;

    const char *ver_section = strstr(buf.data, "\"versions\"");
    if (ver_section) {
        extract_json_string(ver_section, "stable", version, vsz);
    }
    return 1;
}

static void *dep_thread_fn(void *arg) {
    DepArgs *da = (DepArgs *)arg;
    char version[64];
    version[0] = '\0';

    server_log(LOG_INFO, "[MAP] Caut pachet: %s", da->dep);

    if (check_via_repology(da->dep, version, sizeof(version))) {
        send(da->client_fd, "    ", 4, 0);
        send(da->client_fd, da->dep, custom_len(da->dep), 0);
        send(da->client_fd, " \\  # v", 7, 0);
        send(da->client_fd, version, custom_len(version), 0);
        send(da->client_fd, " (repology)\n", 12, 0);
    } else if (check_via_homebrew(da->dep, version, sizeof(version))) {
        send(da->client_fd, "    ", 4, 0);
        send(da->client_fd, da->dep, custom_len(da->dep), 0);
        send(da->client_fd, " \\  # v", 7, 0);
        send(da->client_fd, version, custom_len(version), 0);
        send(da->client_fd, " (homebrew)\n", 12, 0);
    } else {
        const char *fail_start = "    # ESUAT: ";
        const char *fail_end   = " (Lipseste de pe sursele verificate)\n";
        send(da->client_fd, fail_start, custom_len(fail_start), 0);
        send(da->client_fd, da->dep, custom_len(da->dep), 0);
        send(da->client_fd, fail_end, custom_len(fail_end), 0);
    }

    return NULL;
}

// primeste un fisier de la client si il salveaza in directorul uploads/
// format header: "UPLOAD:basename:size" urmat imediat de exact <size> bytes
static void handle_upload(int client_fd, const char *buf, ssize_t buf_len) {
    const char *p = buf + 7;

    char filename[256];
    int fi = 0;
    while (*p && *p != ':' && fi < 255) filename[fi++] = *p++;
    filename[fi] = '\0';

    for (int i = 0; filename[i]; i++) {
        if (filename[i] == '/' || filename[i] == '\\' ||
            (filename[i] == '.' && filename[i + 1] == '.')) {
            const char *err = "ERR:Filename invalid\n===EOF===\n";
            send(client_fd, err, custom_len(err), 0);
            return;
        }
    }

    server_log(LOG_INFO, "[Upload] Inceput: %s", filename);

    if (*p != ':') {
        server_log(LOG_WARN, "[Upload] Format invalid pentru: %s", filename);
        const char *err = "ERR:Format UPLOAD invalid\n===EOF===\n";
        send(client_fd, err, custom_len(err), 0);
        return;
    }
    p++;

    long file_size = 0;
    while (*p >= '0' && *p <= '9') { file_size = file_size * 10 + (*p - '0'); p++; }

    ssize_t header_end = (ssize_t)(p - buf);
    ssize_t extra      = buf_len - header_end;

    char filepath[320];
    int n = snprintf(filepath, sizeof(filepath), "uploads/%s", filename);
    if (n <= 0 || n >= (int)sizeof(filepath)) {
        const char *err = "ERR:Filename prea lung\n===EOF===\n";
        send(client_fd, err, custom_len(err), 0);
        return;
    }

    int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        const char *err = "ERR:Nu pot crea fisierul pe server\n===EOF===\n";
        send(client_fd, err, custom_len(err), 0);
        return;
    }

    long remaining = file_size;

    if (extra > 0) {
        ssize_t to_write = (extra < remaining) ? extra : remaining;
        if (write(fd, p, (size_t)to_write) < 0) perror("write upload");
        remaining -= to_write;
    }

    char recv_buf[4096];
    while (remaining > 0) {
        ssize_t want = (remaining < (long)sizeof(recv_buf)) ? remaining : (long)sizeof(recv_buf);
        ssize_t got  = recv(client_fd, recv_buf, (size_t)want, 0);
        if (got <= 0) break;
        if (write(fd, recv_buf, (size_t)got) < 0) perror("write upload chunk");
        remaining -= got;
    }
    close(fd);

    server_log(LOG_INFO, "[Upload] Complet: %s", filename);
    char resp[320];
    n = snprintf(resp, sizeof(resp), "OK:UPLOAD:%s\n===EOF===\n", filename);
    send(client_fd, resp, (size_t)n, 0);
}

// trimite un fisier din uploads/ catre client
// format cerere: "DOWNLOAD:basename"
static void handle_download(int client_fd, const char *buf) {
    const char *p = buf + 9;

    char filename[256];
    int fi = 0;
    while (*p && fi < 255) filename[fi++] = *p++;
    filename[fi] = '\0';

    server_log(LOG_INFO, "[Download] Cerut: %s", filename);

    for (int i = 0; filename[i]; i++) {
        if (filename[i] == '/' || filename[i] == '\\' ||
            (filename[i] == '.' && filename[i + 1] == '.')) {
            const char *err = "ERR:Filename invalid\n===EOF===\n";
            send(client_fd, err, custom_len(err), 0);
            return;
        }
    }

    char filepath[320];
    int n = snprintf(filepath, sizeof(filepath), "uploads/%s", filename);
    if (n <= 0 || n >= (int)sizeof(filepath)) {
        const char *err = "ERR:Filename prea lung\n===EOF===\n";
        send(client_fd, err, custom_len(err), 0);
        return;
    }

    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        char err[320];
        n = snprintf(err, sizeof(err), "ERR:Fisier inexistent: %s\n===EOF===\n", filename);
        send(client_fd, err, (size_t)n, 0);
        return;
    }

    char read_buf[4096];
    ssize_t got;
    while ((got = read(fd, read_buf, sizeof(read_buf))) > 0) {
        send(client_fd, read_buf, (size_t)got, 0);
    }
    close(fd);
    send(client_fd, "\n===EOF===\n", 11, 0);
}

// returneaza lista fisierelor din uploads/, unul pe linie
// format cerere: "LIST"
static void handle_list(int client_fd) {
    server_log(LOG_INFO, "[List] Cerere lista fisiere");

    DIR *dir = opendir("uploads");
    if (!dir) {
        const char *err = "ERR:Nu pot deschide directorul uploads\n===EOF===\n";
        send(client_fd, err, custom_len(err), 0);
        return;
    }

    struct dirent *entry;
    int found = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        send(client_fd, entry->d_name, custom_len(entry->d_name), 0);
        send(client_fd, "\n", 1, 0);
        found++;
    }
    closedir(dir);

    if (!found) send(client_fd, "(director gol)\n", 15, 0);
    send(client_fd, "===EOF===\n", 10, 0);
}

// sterge un fisier din uploads/
// format cerere: "DELETE:basename"
static void handle_delete(int client_fd, const char *buf) {
    const char *p = buf + 7; // sarim peste "DELETE:"

    char filename[256];
    int fi = 0;
    while (*p && fi < 255) filename[fi++] = *p++;
    filename[fi] = '\0';

    server_log(LOG_INFO, "[Delete] Cerut: %s", filename);

    for (int i = 0; filename[i]; i++) {
        if (filename[i] == '/' || filename[i] == '\\' ||
            (filename[i] == '.' && filename[i + 1] == '.')) {
            const char *err = "ERR:Filename invalid\n===EOF===\n";
            send(client_fd, err, custom_len(err), 0);
            return;
        }
    }

    char filepath[320];
    int n = snprintf(filepath, sizeof(filepath), "uploads/%s", filename);
    if (n <= 0 || n >= (int)sizeof(filepath)) {
        const char *err = "ERR:Filename prea lung\n===EOF===\n";
        send(client_fd, err, custom_len(err), 0);
        return;
    }

    if (unlink(filepath) < 0) {
        char err[320];
        n = snprintf(err, sizeof(err), "ERR:Nu pot sterge: %s\n===EOF===\n", filename);
        send(client_fd, err, (size_t)n, 0);
        return;
    }

    server_log(LOG_INFO, "[Delete] Sters: %s", filename);
    char resp[320];
    n = snprintf(resp, sizeof(resp), "OK:DELETE:%s\n===EOF===\n", filename);
    send(client_fd, resp, (size_t)n, 0);
}

void process_request_and_send(int client_fd, char *request) {
    char deps[10][64];   int dep_count  = 0;
    char envs[10][64];   int env_count  = 0;
    char copies[10][64]; int copy_count = 0;

    for (int i = 0; i < 10; i++) {
        for (int j = 0; j < 64; j++) {
            deps[i][j] = '\0'; envs[i][j] = '\0'; copies[i][j] = '\0';
        }
    }

    char *token = strtok(request, " ");
    while (token != NULL) {
        if (token[0] == 'D' && token[1] == ':') {
            if (dep_count < 10) {
                int j = 0;
                while (token[j + 2] != '\0' && j < 63) { deps[dep_count][j] = token[j + 2]; j++; }
                deps[dep_count][j] = '\0';
                dep_count++;
            }
        } else if (token[0] == 'E' && token[1] == ':') {
            if (env_count < 10) {
                int j = 0;
                while (token[j + 2] != '\0' && j < 63) { envs[env_count][j] = token[j + 2]; j++; }
                envs[env_count][j] = '\0';
                env_count++;
            }
        } else if (token[0] == 'C' && token[1] == ':') {
            if (copy_count < 10) {
                int j = 0;
                while (token[j + 2] != '\0' && j < 63) { copies[copy_count][j] = token[j + 2]; j++; }
                copies[copy_count][j] = '\0';
                copy_count++;
            }
        }
        token = strtok(NULL, " ");
    }

    config_t cfg;
    config_init(&cfg);

    const char *base_image = "ubuntu:22.04";
    const char *maintainer = "admin";
    const char *workdir    = "/app";

    if (config_read_file(&cfg, "demo.cfg")) {
        config_lookup_string(&cfg, "container.base_image", &base_image);
        config_lookup_string(&cfg, "container.maintainer", &maintainer);
        config_lookup_string(&cfg, "container.workdir",    &workdir);
    }

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
            DepArgs da;
            int j = 0;
            while (deps[i][j] && j < 63) { da.dep[j] = deps[i][j]; j++; }
            da.dep[j]    = '\0';
            da.client_fd = client_fd;

            pthread_t tid;
            if (pthread_create(&tid, NULL, dep_thread_fn, &da) != 0) {
                perror("pthread_create dep");
            } else {
                pthread_join(tid, NULL);
            }
            sleep(1); // evitam rate-limit 429 de la Repology
        }

        char *run_end = "    && apt-get clean \\\n    && rm -rf /var/lib/apt/lists/*\n\n";
        send(client_fd, run_end, custom_len(run_end), 0);
    }

    send(client_fd, "CMD [\"/bin/bash\"]\n", 17, 0);
    send(client_fd, "\n===EOF===\n", 11, 0);

    config_destroy(&cfg);
}

// thread UDP pentru comenzile de administrare pe portul ADMIN_PORT
static void *admin_udp_thread(void *arg) {
    (void)arg;

    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) { perror("UDP admin socket"); return NULL; }

    int opt = 1;
    setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in udp_addr;
    udp_addr.sin_family      = AF_INET;
    udp_addr.sin_addr.s_addr = INADDR_ANY;
    udp_addr.sin_port        = htons(ADMIN_PORT);

    if (bind(udp_fd, (struct sockaddr *)&udp_addr, sizeof(udp_addr)) < 0) {
        perror("UDP admin bind");
        close(udp_fd);
        return NULL;
    }

    char cmd[256];
    char resp[2048];
    struct sockaddr_in admin_addr;
    socklen_t admin_len;

    while (1) {
        admin_len = sizeof(admin_addr);
        int nb = (int)recvfrom(udp_fd, cmd, sizeof(cmd) - 1, 0,
                               (struct sockaddr *)&admin_addr, &admin_len);
        if (nb <= 0) continue;
        cmd[nb] = '\0';
        resp[0] = '\0';

        server_log(LOG_INFO, "[Admin] Comanda primita: %s", cmd);

        if (strcmp(cmd, "CMD:STATUS") == 0) {
            time_t now    = time(NULL);
            long   uptime = (long)(now - g_start_time);
            pthread_mutex_lock(&g_clients_mutex);
            int cnt = g_client_count;
            pthread_mutex_unlock(&g_clients_mutex);
            snprintf(resp, sizeof(resp),
                "Status: ONLINE\nPort TCP: %d\nClienti conectati: %d\nUptime: %ld secunde",
                g_tcp_port, cnt, uptime);
        } else if (strcmp(cmd, "CMD:CLIENTS") == 0) {
            pthread_mutex_lock(&g_clients_mutex);
            int cnt = g_client_count;
            int pos = snprintf(resp, sizeof(resp), "Clienti conectati: %d\n", cnt);
            for (int i = 0; i < cnt && pos < (int)sizeof(resp) - 1; i++) {
                pos += snprintf(resp + pos, sizeof(resp) - (size_t)pos,
                    "%d. %s (fd=%d)\n", i + 1, g_clients[i].ip, g_clients[i].fd);
            }
            pthread_mutex_unlock(&g_clients_mutex);
        } else if (strcmp(cmd, "CMD:KICK") == 0) {
            pthread_mutex_lock(&g_clients_mutex);
            if (g_client_count == 0) {
                snprintf(resp, sizeof(resp), "EROARE: Nu exista clienti conectati");
                pthread_mutex_unlock(&g_clients_mutex);
            } else {
                int  kick_fd = g_clients[0].fd;
                char kick_ip[INET_ADDRSTRLEN];
                int  ki = 0;
                while (g_clients[0].ip[ki] && ki < INET_ADDRSTRLEN - 1) {
                    kick_ip[ki] = g_clients[0].ip[ki]; ki++;
                }
                kick_ip[ki] = '\0';
                for (int j = 0; j < g_client_count - 1; j++) g_clients[j] = g_clients[j + 1];
                g_client_count--;
                pthread_mutex_unlock(&g_clients_mutex);
                close(kick_fd);
                snprintf(resp, sizeof(resp), "OK: Client %s (fd=%d) deconectat", kick_ip, kick_fd);
            }
        } else if (strcmp(cmd, "CMD:LOGOUT") == 0) {
            snprintf(resp, sizeof(resp), "OK: Admin deconectat");
        } else {
            snprintf(resp, sizeof(resp), "EROARE: Comanda necunoscuta: %s", cmd);
        }

        sendto(udp_fd, resp, custom_len(resp), 0,
               (struct sockaddr *)&admin_addr, admin_len);
    }

    close(udp_fd);
    return NULL;
}

// thread de procesare: citeste WorkItem din pipe-ul anonim si executa cererea (Nivel B: pipe + pthread)
static void *processing_thread_fn(void *arg) {
    (void)arg;
    WorkItem wi;

    while (1) {
        ssize_t r = read(g_work_pipe[0], &wi, sizeof(wi));
        if (r != (ssize_t)sizeof(wi)) continue;

        server_log(LOG_INFO, "[Procesare] Procesez cerere fd=%d", wi.client_fd);

        if (strncmp(wi.data, "DOWNLOAD:", 9) == 0) {
            handle_download(wi.client_fd, wi.data);
        } else if (strcmp(wi.data, "LIST") == 0) {
            handle_list(wi.client_fd);
        } else if (strncmp(wi.data, "DELETE:", 7) == 0) {
            handle_delete(wi.client_fd, wi.data);
        } else {
            process_request_and_send(wi.client_fd, wi.data);
        }
    }
    return NULL;
}

// thread inotify: monitorizeaza directorul uploads/ pentru modificari (Nivel B: I/O INotify)
static void *inotify_thread_fn(void *arg) {
    (void)arg;

    int ifd = inotify_init();
    if (ifd < 0) { perror("inotify_init"); return NULL; }

    int wd = inotify_add_watch(ifd, "uploads",
                               IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM);
    if (wd < 0) {
        perror("inotify_add_watch");
        close(ifd);
        return NULL;
    }

    // buffer aliniat pentru structurile inotify_event
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

    while (1) {
        ssize_t len = read(ifd, buf, sizeof(buf));
        if (len <= 0) continue;

        char *ptr = buf;
        while (ptr < buf + len) {
            struct inotify_event *ev = (struct inotify_event *)ptr;
            if (ev->len > 0) {
                const char *tip =
                    (ev->mask & IN_CREATE)     ? "CREAT"    :
                    (ev->mask & IN_MODIFY)     ? "MODIF"    :
                    (ev->mask & IN_DELETE)     ? "STERS"    :
                    (ev->mask & IN_MOVED_TO)   ? "MUTAT_IN" :
                    (ev->mask & IN_MOVED_FROM) ? "MUTAT_AF" : "ALTUL";

                server_log(LOG_INFO, "[INotify] uploads/%s: %s", ev->name, tip);
            }
            ptr += sizeof(struct inotify_event) + ev->len;
        }
    }

    inotify_rm_watch(ifd, wd);
    close(ifd);
    return NULL;
}

// ============================================================
// Web/REST API  (FCE Web/API — Component 5, optional)
// Port: WEB_API_PORT
//   GET  /status   -> JSON cu starea serverului
//   GET  /files    -> JSON cu lista fisierelor din uploads/
//   POST /generate -> genereaza Dockerfile din JSON body
// ============================================================

#define WEB_API_PORT 8082

// Parses a JSON array of strings under key, e.g. {"deps":["curl","git"]}
static int parse_json_array(const char *json, const char *key,
                             char out[][64], int max) {
    char pattern[128];
    int pn = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (pn <= 0) return 0;
    const char *pos = strstr(json, pattern);
    if (!pos) return 0;
    pos += pn;
    while (*pos == ' ' || *pos == ':' || *pos == '\t') pos++;
    if (*pos != '[') return 0;
    pos++;

    int count = 0;
    while (count < max && *pos != '\0') {
        while (*pos == ' ' || *pos == ',' || *pos == '\n' || *pos == '\r') pos++;
        if (*pos == ']' || *pos == '\0') break;
        if (*pos != '"') { pos++; continue; }
        pos++;
        int j = 0;
        while (*pos && *pos != '"' && j < 63) out[count][j++] = *pos++;
        out[count][j] = '\0';
        if (*pos == '"') pos++;
        count++;
    }
    return count;
}

// Builds Dockerfile content into out buffer (no socket I/O).
// Reuses check_via_repology / check_via_homebrew from above.
static int generate_dockerfile_to_buf(
    char deps[][64], int dep_count,
    char envs[][64], int env_count,
    char copies[][64], int copy_count,
    char *out, int out_sz)
{
    config_t cfg;
    config_init(&cfg);
    const char *base_image = "ubuntu:22.04";
    const char *maintainer = "admin";
    const char *workdir    = "/app";
    if (config_read_file(&cfg, "demo.cfg")) {
        config_lookup_string(&cfg, "container.base_image", &base_image);
        config_lookup_string(&cfg, "container.maintainer", &maintainer);
        config_lookup_string(&cfg, "container.workdir",    &workdir);
    }

    int pos = 0;
    pos += snprintf(out + pos, (size_t)(out_sz - pos),
        "FROM %s\nLABEL maintainer=\"%s\"\nWORKDIR %s\n\n",
        base_image, maintainer, workdir);

    for (int i = 0; i < env_count && pos < out_sz - 1; i++)
        pos += snprintf(out + pos, (size_t)(out_sz - pos), "ENV %s\n", envs[i]);
    if (env_count > 0 && pos < out_sz - 1)
        pos += snprintf(out + pos, (size_t)(out_sz - pos), "\n");

    for (int i = 0; i < copy_count && pos < out_sz - 1; i++)
        pos += snprintf(out + pos, (size_t)(out_sz - pos), "COPY %s\n", copies[i]);
    if (copy_count > 0 && pos < out_sz - 1)
        pos += snprintf(out + pos, (size_t)(out_sz - pos), "\n");

    if (dep_count > 0) {
        pos += snprintf(out + pos, (size_t)(out_sz - pos),
            "RUN apt-get update && apt-get install -y \\\n");
        for (int i = 0; i < dep_count && pos < out_sz - 1; i++) {
            char version[64] = {0};
            server_log(LOG_INFO, "[WebAPI/generate] Caut: %s", deps[i]);
            if (check_via_repology(deps[i], version, sizeof(version))) {
                pos += snprintf(out + pos, (size_t)(out_sz - pos),
                    "    %s \\  # v%s (repology)\n", deps[i], version);
            } else if (check_via_homebrew(deps[i], version, sizeof(version))) {
                pos += snprintf(out + pos, (size_t)(out_sz - pos),
                    "    %s \\  # v%s (homebrew)\n", deps[i], version);
            } else {
                pos += snprintf(out + pos, (size_t)(out_sz - pos),
                    "    # ESUAT: %s\n", deps[i]);
            }
            sleep(1);
        }
        pos += snprintf(out + pos, (size_t)(out_sz - pos),
            "    && apt-get clean && rm -rf /var/lib/apt/lists/*\n\n");
    }

    if (pos < out_sz - 1)
        pos += snprintf(out + pos, (size_t)(out_sz - pos), "CMD [\"/bin/bash\"]\n");

    config_destroy(&cfg);
    return pos;
}

// Trimite un raspuns HTTP complet (headere + body)
static void http_send(int fd, int code, const char *ctype,
                      const char *body, int body_len) {
    const char *reason = (code == 200) ? "OK" :
                         (code == 404) ? "Not Found" : "Bad Request";
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        code, reason, ctype, body_len);
    send(fd, hdr, (size_t)n, 0);
    if (body_len > 0) send(fd, body, (size_t)body_len, 0);
}

// Parseaza o cerere HTTP: metoda, cale, body
// Returneaza 0 la esec
static int parse_http_request(int fd, char method[16], char path[256],
                               char *body, int body_sz, int *body_out_len) {
    char raw[16384] = {0};
    int  raw_len    = 0;

    while (raw_len < (int)sizeof(raw) - 1) {
        ssize_t r = recv(fd, raw + raw_len,
                         (size_t)(sizeof(raw) - 1 - raw_len), 0);
        if (r <= 0) break;
        raw_len += (int)r;
        raw[raw_len] = '\0';
        if (strstr(raw, "\r\n\r\n")) break;
    }
    if (raw_len == 0) return 0;

    // Prima linie: "METHOD /path HTTP/1.x"
    char *p = raw;
    int mi = 0;
    while (*p && *p != ' ' && mi < 15) method[mi++] = *p++;
    method[mi] = '\0';
    while (*p == ' ') p++;
    int pi = 0;
    while (*p && *p != ' ' && pi < 255) path[pi++] = *p++;
    path[pi] = '\0';

    // Content-Length
    int content_length = 0;
    const char *cl = strstr(raw, "Content-Length:");
    if (!cl) cl = strstr(raw, "content-length:");
    if (cl) {
        cl += 15;
        while (*cl == ' ') cl++;
        while (*cl >= '0' && *cl <= '9') {
            content_length = content_length * 10 + (*cl - '0');
            cl++;
        }
    }

    // Body
    *body_out_len = 0;
    const char *hdr_end = strstr(raw, "\r\n\r\n");
    if (hdr_end && content_length > 0) {
        hdr_end += 4;
        int already = raw_len - (int)(hdr_end - raw);
        int copy    = already < body_sz - 1 ? already : body_sz - 1;
        for (int i = 0; i < copy; i++) body[i] = hdr_end[i];
        *body_out_len = copy;

        int remaining = content_length - already;
        while (remaining > 0 && *body_out_len < body_sz - 1) {
            int want = remaining < body_sz - 1 - *body_out_len
                     ? remaining : body_sz - 1 - *body_out_len;
            ssize_t r = recv(fd, body + *body_out_len, (size_t)want, 0);
            if (r <= 0) break;
            *body_out_len += (int)r;
            remaining    -= (int)r;
        }
        body[*body_out_len] = '\0';
    }
    return 1;
}

// GET /status
static void web_handle_status(int fd) {
    time_t now    = time(NULL);
    long   uptime = (long)(now - g_start_time);

    pthread_mutex_lock(&g_clients_mutex);
    int cnt = g_client_count;
    pthread_mutex_unlock(&g_clients_mutex);

    char json[512];
    int n = snprintf(json, sizeof(json),
        "{\"status\":\"ONLINE\",\"tcp_port\":%d,\"admin_port\":%d,"
        "\"web_port\":%d,\"clients\":%d,\"uptime\":%ld}",
        g_tcp_port, ADMIN_PORT, WEB_API_PORT, cnt, uptime);

    http_send(fd, 200, "application/json", json, n);
}

// GET /files  — lista fisierelor din uploads/
static void web_handle_files(int fd) {
    DIR  *dir = opendir("uploads");
    char  json[4096];
    int   pos = snprintf(json, sizeof(json), "{\"files\":[");

    if (dir) {
        struct dirent *entry;
        int first = 1;
        while ((entry = readdir(dir)) != NULL && pos < (int)sizeof(json) - 8) {
            if (entry->d_name[0] == '.') continue;
            if (!first) pos += snprintf(json + pos, sizeof(json) - (size_t)pos, ",");
            pos += snprintf(json + pos, sizeof(json) - (size_t)pos,
                            "\"%s\"", entry->d_name);
            first = 0;
        }
        closedir(dir);
    }
    pos += snprintf(json + pos, sizeof(json) - (size_t)pos, "]}");
    http_send(fd, 200, "application/json", json, pos);
}

// POST /generate  — genereaza Dockerfile din JSON body
static void web_handle_generate(int fd, const char *body) {
    char deps[10][64];   int dep_count  = 0;
    char envs[10][64];   int env_count  = 0;
    char copies[10][64]; int copy_count = 0;

    dep_count  = parse_json_array(body, "deps",   deps,   10);
    env_count  = parse_json_array(body, "envs",   envs,   10);
    copy_count = parse_json_array(body, "copies", copies, 10);

    server_log(LOG_INFO, "[WebAPI] /generate: %d dep, %d env, %d copy",
               dep_count, env_count, copy_count);

    char dockerfile[16384];
    int  len = generate_dockerfile_to_buf(
        deps, dep_count, envs, env_count, copies, copy_count,
        dockerfile, (int)sizeof(dockerfile));

    http_send(fd, 200, "text/plain", dockerfile, len);
}

// Thread per conexiune HTTP (spawn-uit din web_api_thread)
static void *web_conn_thread(void *arg) {
    int fd = *(int *)arg;
    free(arg);

    char method[16]  = {0};
    char path[256]   = {0};
    char body[16384] = {0};
    int  body_len    = 0;

    if (!parse_http_request(fd, method, path, body, (int)sizeof(body), &body_len)) {
        close(fd);
        return NULL;
    }

    server_log(LOG_INFO, "[WebAPI] %s %s (%d bytes body)", method, path, body_len);

    if (strcmp(method, "GET") == 0 && strcmp(path, "/status") == 0) {
        web_handle_status(fd);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/files") == 0) {
        web_handle_files(fd);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/generate") == 0) {
        web_handle_generate(fd, body);
    } else {
        const char *err = "{\"error\":\"Not Found\"}";
        http_send(fd, 404, "application/json", err, (int)strlen(err));
    }

    close(fd);
    return NULL;
}

// Thread principal Web/REST API — asculta conexiuni HTTP pe WEB_API_PORT
static void *web_api_thread(void *arg) {
    (void)arg;

    int web_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (web_fd < 0) { perror("web socket"); return NULL; }

    int opt = 1;
    setsockopt(web_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(WEB_API_PORT);

    if (bind(web_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("web bind");
        close(web_fd);
        return NULL;
    }
    listen(web_fd, 10);
    server_log(LOG_INFO, "[WebAPI] HTTP server pornit pe port %d", WEB_API_PORT);

    while (1) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(web_fd, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) continue;

        int *fdp = malloc(sizeof(int));
        if (!fdp) { close(cfd); continue; }
        *fdp = cfd;

        pthread_t tid;
        if (pthread_create(&tid, NULL, web_conn_thread, fdp) != 0) {
            close(cfd); free(fdp);
        } else {
            pthread_detach(tid);
        }
    }

    close(web_fd);
    return NULL;
}

void load_env_file(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (fp == NULL) return;

    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (line[0] == '\n' || line[0] == '#' || line[0] == '\r') continue;

        char *separator = strchr(line, '=');
        if (separator != NULL) {
            *separator = '\0';
            char *key   = line;
            char *value = separator + 1;

            size_t len = custom_len(value);
            if (len > 0 && value[len - 1] == '\n') value[len - 1] = '\0';
            len = custom_len(value);
            if (len > 0 && value[len - 1] == '\r') value[len - 1] = '\0';

            setenv(key, value, 1);
        }
    }
    fclose(fp);
}

// thread stdin: accepta comenzi interactive de la tastatura (Server IN)
static void *stdin_thread_fn(void *arg) {
    (void)arg;
    char line[256];

    server_log(LOG_INFO, "[Stdin] Comenzi: help | status | clients | shutdown");

    while (g_running) {
        if (write(STDOUT_FILENO, "server> ", 8) < 0) break;

        ssize_t n = read(STDIN_FILENO, line, sizeof(line) - 1);
        if (n <= 0) break;
        line[n] = '\0';
        if (n > 0 && line[n - 1] == '\n') line[n - 1] = '\0';
        if (n > 1 && line[n - 2] == '\r') line[n - 2] = '\0';
        if (line[0] == '\0') continue;

        if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
            server_log(LOG_INFO, "[Stdin] Comenzi disponibile:");
            server_log(LOG_INFO, "[Stdin]   status   - uptime, porturi, clienti");
            server_log(LOG_INFO, "[Stdin]   clients  - lista clientilor conectati");
            server_log(LOG_INFO, "[Stdin]   shutdown - oprire eleganta a serverului");

        } else if (strcmp(line, "status") == 0) {
            time_t now    = time(NULL);
            long   uptime = (long)(now - g_start_time);
            pthread_mutex_lock(&g_clients_mutex);
            int cnt = g_client_count;
            pthread_mutex_unlock(&g_clients_mutex);
            server_log(LOG_INFO, "[Stdin] Status: ONLINE | TCP:%d | Admin(UDP):%d | REST:%d | Clienti:%d | Uptime:%lds",
                       g_tcp_port, ADMIN_PORT, WEB_API_PORT, cnt, uptime);

        } else if (strcmp(line, "clients") == 0) {
            pthread_mutex_lock(&g_clients_mutex);
            if (g_client_count == 0) {
                server_log(LOG_INFO, "[Stdin] Niciun client conectat.");
            } else {
                server_log(LOG_INFO, "[Stdin] %d client(i) conectat(i):", g_client_count);
                for (int i = 0; i < g_client_count; i++)
                    server_log(LOG_INFO, "[Stdin]   %d. %s (fd=%d)",
                               i + 1, g_clients[i].ip, g_clients[i].fd);
            }
            pthread_mutex_unlock(&g_clients_mutex);

        } else if (strcmp(line, "shutdown") == 0) {
            server_log(LOG_INFO, "[Stdin] Shutdown initiat de la consola.");
            g_running = 0;
            break;

        } else {
            server_log(LOG_WARN, "[Stdin] Comanda necunoscuta: '%s'. Tastati 'help'.", line);
        }
    }
    return NULL;
}

int main() {
    load_env_file(".env");

    int port = 8080;
    char *env_port = getenv("SERVER_PORT");
    if (env_port != NULL) {
        char *endptr;
        long val = strtol(env_port, &endptr, 10);
        if (*endptr == '\0' && val > 0 && val <= 65535) port = (int)val;
    }
    g_tcp_port   = port;
    g_start_time = time(NULL);

    g_log_file = fopen(LOG_FILE, "a");
    if (!g_log_file) perror("fopen server.log");

    signal(SIGINT,  sigint_handler);
    signal(SIGTERM, sigint_handler);

    // cream pipe-ul anonim pentru coada FIFO (Nivel B)
    if (pipe(g_work_pipe) < 0) { perror("pipe"); return 1; }

    int server_fd;
    struct sockaddr_in address;
    int opt = 1;

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) return 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port        = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) return 1;
    listen(server_fd, 10);

    mkdir("uploads", 0755);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    server_log(LOG_INFO, "================================================");
    server_log(LOG_INFO, "  Dockerfile Generator Server");
    server_log(LOG_INFO, "  TCP clients  : port %d", port);
    server_log(LOG_INFO, "  Admin UDP    : port %d", ADMIN_PORT);
    server_log(LOG_INFO, "  REST HTTP    : port %d", WEB_API_PORT);
    server_log(LOG_INFO, "  Log file     : %s",      LOG_FILE);
    server_log(LOG_INFO, "  Nivel B: poll | pipe anonim | inotify | pthread");
    server_log(LOG_INFO, "================================================");

    // pornim threadurile de servicii (Nivel B: exclusiv pthread pentru sincronizare)
    pthread_t admin_tid, proc_tid, inotify_tid, web_tid, stdin_tid;

    if (pthread_create(&admin_tid, NULL, admin_udp_thread, NULL) != 0)
        perror("pthread_create admin");
    else
        pthread_detach(admin_tid);

    if (pthread_create(&proc_tid, NULL, processing_thread_fn, NULL) != 0)
        perror("pthread_create procesare");
    else
        pthread_detach(proc_tid);

    if (pthread_create(&inotify_tid, NULL, inotify_thread_fn, NULL) != 0)
        perror("pthread_create inotify");
    else
        pthread_detach(inotify_tid);

    if (pthread_create(&web_tid, NULL, web_api_thread, NULL) != 0)
        perror("pthread_create web");
    else
        pthread_detach(web_tid);

    if (pthread_create(&stdin_tid, NULL, stdin_thread_fn, NULL) != 0)
        perror("pthread_create stdin");
    else
        pthread_detach(stdin_tid);

    // I/O multiplexing cu poll() pentru toti clientii simultan (Nivel B: poll/select)
    struct pollfd poll_fds[MAX_CLIENTS + 1];
    int           poll_count = 1;

    poll_fds[0].fd      = server_fd;
    poll_fds[0].events  = POLLIN;
    poll_fds[0].revents = 0;

    while (g_running) {
        int ready = poll(poll_fds, (nfds_t)poll_count, 500);
        if (ready < 0) {
            if (errno == EINTR) continue; // intrerupt de semnal, verificam g_running
            perror("poll"); continue;
        }
        if (ready == 0) continue; // timeout — verificam g_running si continuam

        // server fd: conexiune noua
        if (poll_fds[0].revents & POLLIN) {
            struct sockaddr_in caddr;
            socklen_t clen = sizeof(caddr);
            int cfd = accept(server_fd, (struct sockaddr *)&caddr, &clen);
            if (cfd >= 0) {
                if (poll_count < MAX_CLIENTS + 1) {
                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &caddr.sin_addr, ip, sizeof(ip));
                    client_register(cfd, ip);

                    poll_fds[poll_count].fd      = cfd;
                    poll_fds[poll_count].events  = POLLIN;
                    poll_fds[poll_count].revents = 0;
                    poll_count++;

                    server_log(LOG_INFO, "[Server] Client acceptat: %s fd=%d", ip, cfd);
                } else {
                    close(cfd);
                }
            }
        }

        // clienti existenti: date disponibile
        for (int i = 1; i < poll_count; i++) {
            if (!(poll_fds[i].revents & POLLIN)) continue;

            char buf[WORK_DATA_SIZE];
            ssize_t bytes = recv(poll_fds[i].fd, buf, sizeof(buf) - 1, 0);

            if (bytes <= 0) {
                server_log(LOG_INFO, "[Server] Client deconectat fd=%d", poll_fds[i].fd);
                client_unregister(poll_fds[i].fd);
                close(poll_fds[i].fd);
                poll_fds[i] = poll_fds[poll_count - 1];
                poll_count--;
                i--;
                continue;
            }
            buf[bytes] = '\0';

            if (strncmp(buf, "UPLOAD:", 7) == 0) {
                // upload tratat inline in I/O thread (streaming de date)
                handle_upload(poll_fds[i].fd, buf, bytes);
            } else {
                // orice alta cerere intra in coada FIFO via pipe anonim
                WorkItem wi;
                wi.client_fd = poll_fds[i].fd;
                wi.data_len  = bytes;
                for (int j = 0; j < bytes && j < WORK_DATA_SIZE - 1; j++)
                    wi.data[j] = buf[j];
                wi.data[bytes < WORK_DATA_SIZE ? bytes : WORK_DATA_SIZE - 1] = '\0';

                if (write(g_work_pipe[1], &wi, sizeof(wi)) < 0)
                    perror("write pipe");
            }
        }
    }

    server_log(LOG_INFO, "[Server] Oprire curata (g_running=%d).", (int)g_running);
    close(server_fd);
    close(g_work_pipe[0]);
    close(g_work_pipe[1]);
    curl_global_cleanup();
    if (g_log_file) { fclose(g_log_file); g_log_file = NULL; }
    return 0;
}
