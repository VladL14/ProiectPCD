


/**
 * P8
 * Proiect PCD - Dockerfile Generator (Server)
 * aici avem implementarea serverului. ideea de baza este ca serverul sa primeasca comenzi
 * de la mai multi clienti tcp simultan si sa asambleze un dockerfile pentru ei.
 * de asemenea comunica printr un socket udp cu panoul de admin pentru interogari.
 * inotify monitorizeaza folderul de uploads, iar pentru cozi si date comune folosim pipe uri si mutex uri.
 */

#include <stdio.h>       // perror si operatii de baza cu fisiere
#include <stdlib.h>      // alocare de memorie si functii de baza standard
#include <string.h>      // strcmp, strtok, procesare de texte
#include <unistd.h>      // read, write, close, sleep
#include <fcntl.h>       // deschidere de fisiere cu O_WRONLY
#include <sys/stat.h>    // mkdir si citit info despre fisiere
#include <sys/inotify.h> // inotify ca sa pazim folderul de uploads sa vedem ce misca
#include <dirent.h>      // parcurgere foldere
#include <poll.h>        // multiplexare i/o ca sa ascultam toti clientii tcp deodata
#include <pthread.h>     // thread uri si mutex uri ca sa nu ne batem pe aceleasi date
#include <sys/socket.h>  // socket, bind, listen, accept
#include <netinet/in.h>  // familii de adrese pentru porturi ipv4
#include <arpa/inet.h>   // convertim adrese ip pentru retea
#include <time.h>        // ne uitam la ceas ca sa calculam uptime ul
#include <curl/curl.h>   // facem cereri web catre repology si homebrew
#include <libconfig.h>   // citim valorile initiale din cfg ul nostru
#include <signal.h>      // prindem semnale gen ctrl c ca sa inchidem totul curat
#include <errno.h>       // sa aflam exact codul erorii

#define CURL_BUFFER_SIZE 8192
#define ADMIN_PORT       8081
#define MAX_CLIENTS      64
#define WORK_DATA_SIZE   1024
#define LOG_FILE         "server.log"

#define MAX_LOG_SIZE 1024
#define MAX_PATH_SIZE 512
#define MAX_BUFFER_SIZE 2048
#define POLL_TIMEOUT 500
#define REPOLOGY_DELAY 1
#define ADMIN_TIMEOUT_SEC 60


static size_t custom_len(const char *str) {
    size_t i = 0;
    while (str[i] != '\0') i++;
    return i;
}

static void custom_itoa(long n, char* buf) {

    if (n == 0) {
        buf[0] = '0'; buf[1] = '\0'; return;
    }
    long temp = n; int len = 0;
    if (n < 0) { temp = -temp; len++; }
    while (temp > 0) { len++; temp /= 10; }
    buf[len] = '\0';
    temp = n < 0 ? -n : n;
    for (int i = len - 1; i >= (n < 0 ? 1 : 0); i--) {
        buf[i] = (char)((temp % 10) + '0');
        temp /= 10;
    }
    if (n < 0) buf[0] = '-';
}

static void custom_concat(char* dest, const char* src) {
    int i = 0; while (dest[i] != '\0') i++;
    int j = 0; while (src[j] != '\0') { dest[i++] = src[j++]; }
    dest[i] = '\0';
}

static void custom_strcpy(char* dest, const char* src) {
    int i = 0; while (src[i] != '\0') { dest[i] = src[i]; i++; }
    dest[i] = '\0';
}

// sistemul de log uri thread safe ca sa nu se incalice textul pe ecran cand scriu mai multe thread uri deodata

typedef enum { LOG_INFO, LOG_WARN, LOG_ERROR } LogLevel;

static FILE           *g_log_file  = NULL;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static void server_log(LogLevel level, const char *msg) {
    time_t     now     = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[24];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    const char *lvl = (level == LOG_INFO)  ? "INFO " :
                      (level == LOG_WARN)  ? "WARN " : "ERROR";

    char line[MAX_LOG_SIZE];
    custom_strcpy(line, "["); custom_concat(line, timestamp); custom_concat(line, "] [");
    custom_concat(line, lvl); custom_concat(line, "] ");
    custom_concat(line, msg); custom_concat(line, "\n");

    pthread_mutex_lock(&g_log_mutex);
    if (write(STDOUT_FILENO, line, custom_len(line)) < 0) {}
    if (g_log_file) {
        fwrite(line, 1, custom_len(line), g_log_file);
        fflush(g_log_file);
    }
    pthread_mutex_unlock(&g_log_mutex);
}

// pipe ul anonim: thread ul principal arunca chestii in el, iar thread ul de procesare le scoate si face treaba
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

// Starea sesiunii de administrare (protejata de un mutex dedicat)
static struct sockaddr_in g_current_admin_addr;
static time_t             g_admin_last_activity = 0;
static pthread_mutex_t    g_admin_mutex = PTHREAD_MUTEX_INITIALIZER;

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

    char mm[MAX_LOG_SIZE]; custom_strcpy(mm, "[MAP] Caut pachet: "); custom_concat(mm, da->dep); server_log(LOG_INFO, mm);

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

    char um[MAX_LOG_SIZE]; custom_strcpy(um, "[Upload] Inceput: "); custom_concat(um, filename); server_log(LOG_INFO, um);

    if (*p != ':') {
        char um2[MAX_LOG_SIZE]; custom_strcpy(um2, "[Upload] Format invalid pentru: "); custom_concat(um2, filename); server_log(LOG_WARN, um2);
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
    custom_strcpy(filepath, "uploads/"); custom_concat(filepath, filename);
    int n = custom_len(filepath);
    if (n <= 0 || n >= MAX_PATH_SIZE) {
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

    char um3[MAX_LOG_SIZE]; custom_strcpy(um3, "[Upload] Complet: "); custom_concat(um3, filename); server_log(LOG_INFO, um3);
    char resp[320];
    custom_strcpy(resp, "OK:UPLOAD:"); custom_concat(resp, filename); custom_concat(resp, "\n===EOF===\n");
            n = custom_len(resp);
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

    char dm[MAX_LOG_SIZE]; custom_strcpy(dm, "[Download] Cerut: "); custom_concat(dm, filename); server_log(LOG_INFO, dm);

    for (int i = 0; filename[i]; i++) {
        if (filename[i] == '/' || filename[i] == '\\' ||
            (filename[i] == '.' && filename[i + 1] == '.')) {
            const char *err = "ERR:Filename invalid\n===EOF===\n";
            send(client_fd, err, custom_len(err), 0);
            return;
        }
    }

    char filepath[320];
    custom_strcpy(filepath, "uploads/"); custom_concat(filepath, filename);
    int n = custom_len(filepath);
    if (n <= 0 || n >= MAX_PATH_SIZE) {
        const char *err = "ERR:Filename prea lung\n===EOF===\n";
        send(client_fd, err, custom_len(err), 0);
        return;
    }

    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        char err[320];
        custom_strcpy(err, "ERR:Fisier inexistent: "); custom_concat(err, filename); custom_concat(err, "\n===EOF===\n");
            n = custom_len(err);
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

    char del[MAX_LOG_SIZE]; custom_strcpy(del, "[Delete] Cerut: "); custom_concat(del, filename); server_log(LOG_INFO, del);

    for (int i = 0; filename[i]; i++) {
        if (filename[i] == '/' || filename[i] == '\\' ||
            (filename[i] == '.' && filename[i + 1] == '.')) {
            const char *err = "ERR:Filename invalid\n===EOF===\n";
            send(client_fd, err, custom_len(err), 0);
            return;
        }
    }

    char filepath[320];
    custom_strcpy(filepath, "uploads/"); custom_concat(filepath, filename);
    int n = custom_len(filepath);
    if (n <= 0 || n >= MAX_PATH_SIZE) {
        const char *err = "ERR:Filename prea lung\n===EOF===\n";
        send(client_fd, err, custom_len(err), 0);
        return;
    }

    if (unlink(filepath) < 0) {
        char err[320];
        custom_strcpy(err, "ERR:Nu pot sterge: "); custom_concat(err, filename); custom_concat(err, "\n===EOF===\n");
            n = custom_len(err);
        send(client_fd, err, (size_t)n, 0);
        return;
    }

    char del2[MAX_LOG_SIZE]; custom_strcpy(del2, "[Delete] Sters: "); custom_concat(del2, filename); server_log(LOG_INFO, del2);
    char resp[320];
    custom_strcpy(resp, "OK:DELETE:"); custom_concat(resp, filename); custom_concat(resp, "\n===EOF===\n");
            n = custom_len(resp);
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
            sleep(REPOLOGY_DELAY); // evitam rate-limit 429 de la Repology
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
    if (setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, (socklen_t)sizeof(opt)) < 0) {
        perror("setsockopt admin");
    }

    struct sockaddr_in udp_addr = {0};
    udp_addr.sin_family      = AF_INET;
    udp_addr.sin_addr.s_addr = INADDR_ANY;
    udp_addr.sin_port        = htons(ADMIN_PORT);

    if (bind(udp_fd, (struct sockaddr *)&udp_addr, (socklen_t)sizeof(udp_addr)) < 0) {
        perror("UDP admin bind");
        close(udp_fd);
        return NULL;
    }

    char cmd[256] = {0};
    char resp[2048] = {0};
    struct sockaddr_in admin_addr = {0};
    socklen_t admin_len = 0;

    while (1) {
        admin_len = (socklen_t)sizeof(admin_addr);
        ssize_t nb_raw = recvfrom(udp_fd, cmd, sizeof(cmd) - 1, 0,
                                  (struct sockaddr *)&admin_addr, &admin_len);
        if (nb_raw <= 0) continue;
        
        size_t nb = (size_t)nb_raw;
        cmd[nb] = '\0';
        resp[0] = '\0';

        time_t now = time(NULL);
        int is_authorized = 0;

        // Logica 1:1 - Verificare si validare sesiune admin
        pthread_mutex_lock(&g_admin_mutex);
        if (g_admin_last_activity == 0 || (now - g_admin_last_activity) > ADMIN_TIMEOUT_SEC) {
            g_current_admin_addr = admin_addr;
            g_admin_last_activity = now;
            is_authorized = 1;
            server_log(LOG_INFO, "[Admin] Sesiune noua de administrare preluata.");
        } else {
            if (g_current_admin_addr.sin_addr.s_addr == admin_addr.sin_addr.s_addr &&
                g_current_admin_addr.sin_port == admin_addr.sin_port) {
                g_admin_last_activity = now;
                is_authorized = 1;
            }
        }
        pthread_mutex_unlock(&g_admin_mutex);

        // Respingere pachet daca nu este autorizat
        if (!is_authorized) {
            const char *err_msg = "EROARE: Serverul este deja administrat de altcineva. Reincercati mai tarziu.";
            sendto(udp_fd, err_msg, custom_len(err_msg), 0,
                   (struct sockaddr *)&admin_addr, admin_len);
            server_log(LOG_WARN, "[Admin] Tentativa de acces respinsa (sesiune ocupata).");
            continue;
        }

        // Pregatim mesajul pentru log folosind functiile custom
        char log_buf[512] = {0};
        custom_strcpy(log_buf, "[Admin] Comanda primita: ");
        custom_concat(log_buf, cmd);
        server_log(LOG_INFO, log_buf);

        // Procesare comenzi admin exclusiv cu custom_concat
        char num_buf[32] = {0}; // Buffer temporar pentru numere

        if (strcmp(cmd, "CMD:STATUS") == 0) {
            time_t current_time = time(NULL);
            long   uptime = (long)(current_time - g_start_time);
            
            pthread_mutex_lock(&g_clients_mutex);
            int cnt = g_client_count;
            pthread_mutex_unlock(&g_clients_mutex);
            
            custom_strcpy(resp, "Status: ONLINE\nPort TCP: ");
            custom_itoa((long)g_tcp_port, num_buf);
            custom_concat(resp, num_buf);
            
            custom_concat(resp, "\nClienti conectati: ");
            custom_itoa((long)cnt, num_buf);
            custom_concat(resp, num_buf);
            
            custom_concat(resp, "\nUptime: ");
            custom_itoa(uptime, num_buf);
            custom_concat(resp, num_buf);
            custom_concat(resp, " secunde");
            
        } else if (strcmp(cmd, "CMD:CLIENTS") == 0) {
            pthread_mutex_lock(&g_clients_mutex);
            int cnt = g_client_count;
            
            custom_strcpy(resp, "Clienti conectati: ");
            custom_itoa((long)cnt, num_buf);
            custom_concat(resp, num_buf);
            custom_concat(resp, "\n");
            
            for (int i = 0; i < cnt; i++) {
                custom_itoa((long)(i + 1), num_buf);
                custom_concat(resp, num_buf);
                custom_concat(resp, ". ");
                
                custom_concat(resp, g_clients[i].ip);
                
                custom_concat(resp, " (fd=");
                custom_itoa((long)g_clients[i].fd, num_buf);
                custom_concat(resp, num_buf);
                custom_concat(resp, ")\n");
            }
            pthread_mutex_unlock(&g_clients_mutex);
            
        } else if (strcmp(cmd, "CMD:KICK") == 0) {
            pthread_mutex_lock(&g_clients_mutex);
            if (g_client_count == 0) {
                custom_strcpy(resp, "EROARE: Nu exista clienti conectati");
                pthread_mutex_unlock(&g_clients_mutex);
            } else {
                int  kick_fd = g_clients[0].fd;
                char kick_ip[INET_ADDRSTRLEN] = {0};
                int  ki = 0;
                while (g_clients[0].ip[ki] && ki < INET_ADDRSTRLEN - 1) {
                    kick_ip[ki] = g_clients[0].ip[ki]; ki++;
                }
                kick_ip[ki] = '\0';
                
                for (int j = 0; j < g_client_count - 1; j++) {
                    g_clients[j] = g_clients[j + 1];
                }
                g_client_count--;
                pthread_mutex_unlock(&g_clients_mutex);
                close(kick_fd);
                
                custom_strcpy(resp, "OK: Client ");
                custom_concat(resp, kick_ip);
                custom_concat(resp, " (fd=");
                custom_itoa((long)kick_fd, num_buf);
                custom_concat(resp, num_buf);
                custom_concat(resp, ") deconectat");
            }
            
        } else if (strcmp(cmd, "CMD:LOGOUT") == 0) {
            pthread_mutex_lock(&g_admin_mutex);
            g_admin_last_activity = 0;
            g_current_admin_addr.sin_family = 0;
            g_current_admin_addr.sin_port = 0;
            g_current_admin_addr.sin_addr.s_addr = 0;
            pthread_mutex_unlock(&g_admin_mutex);
            
            custom_strcpy(resp, "OK: Admin deconectat");
            
        } else if (strcmp(cmd, "CMD:VERSION") == 0) {
            custom_strcpy(resp, "Versiune Server: DockerGen v1.0\nStatus: Activ");
            
        } else if (strcmp(cmd, "CMD:PING") == 0) {
            custom_strcpy(resp, "PONG! Serverul functioneaza normal.");
            
        } else if (strcmp(cmd, "CMD:CLEAN") == 0) {
            DIR *dir = opendir("uploads");
            if (dir) {
                struct dirent *en;
                int count = 0;
                while ((en = readdir(dir)) != NULL) {
                    if (en->d_name[0] != '.') {
                        char p[300] = {0};
                        custom_strcpy(p, "uploads/");
                        custom_concat(p, en->d_name);
                        unlink(p);
                        count++;
                    }
                }
                closedir(dir);
                
                custom_strcpy(resp, "OK: ");
                custom_itoa((long)count, num_buf);
                custom_concat(resp, num_buf);
                custom_concat(resp, " fisiere sterse din uploads/");
            } else {
                custom_strcpy(resp, "EROARE: Nu pot deschide directorul uploads/");
            }
            
        } else {
            custom_strcpy(resp, "EROARE: Comanda necunoscuta: ");
            custom_concat(resp, cmd);
        }

        // Trimitere raspuns catre clientul care a cerut
        ssize_t sent = sendto(udp_fd, resp, custom_len(resp), 0,
                              (struct sockaddr *)&admin_addr, admin_len);
        if (sent < 0) {
            perror("sendto admin response");
        }
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

        char pm[MAX_LOG_SIZE]; char tmp[32]; custom_itoa(wi.client_fd, tmp); custom_strcpy(pm, "[Procesare] Procesez cerere fd="); custom_concat(pm, tmp); server_log(LOG_INFO, pm);

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
                               IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM | IN_CLOSE_WRITE);
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
                    (ev->mask & IN_CLOSE_WRITE)? "FINALIZAT_SCRIERE" :
                    (ev->mask & IN_CREATE)     ? "CREAT"    :
                    (ev->mask & IN_MODIFY)     ? "MODIF"    :
                    (ev->mask & IN_DELETE)     ? "STERS"    :
                    (ev->mask & IN_MOVED_TO)   ? "MUTAT_IN" :
                    (ev->mask & IN_MOVED_FROM) ? "MUTAT_AF" : "ALTUL";

                char msg[MAX_LOG_SIZE]; custom_strcpy(msg, "[INotify] uploads/"); custom_concat(msg, ev->name); custom_concat(msg, ": "); custom_concat(msg, tip);
                server_log(LOG_INFO, msg);
            }
            ptr += sizeof(struct inotify_event) + ev->len;
        }
    }

    inotify_rm_watch(ifd, wd);
    close(ifd);
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
            char st_msg[MAX_LOG_SIZE]; char tmp3[32];
            custom_strcpy(st_msg, "[Stdin] Status: ONLINE | TCP:"); custom_itoa(g_tcp_port, tmp3); custom_concat(st_msg, tmp3);
            custom_concat(st_msg, " | Admin(UDP):"); custom_itoa(ADMIN_PORT, tmp3); custom_concat(st_msg, tmp3);
            custom_concat(st_msg, " | Clienti:"); custom_itoa(cnt, tmp3); custom_concat(st_msg, tmp3);
            custom_concat(st_msg, " | Uptime:"); custom_itoa(uptime, tmp3); custom_concat(st_msg, tmp3); custom_concat(st_msg, "s");
            server_log(LOG_INFO, st_msg);

        } else if (strcmp(line, "clients") == 0) {
            pthread_mutex_lock(&g_clients_mutex);
            if (g_client_count == 0) {
                server_log(LOG_INFO, "[Stdin] Niciun client conectat.");
            } else {
                char cl_msg[MAX_LOG_SIZE]; char tmp4[32]; custom_itoa(g_client_count, tmp4);
                custom_strcpy(cl_msg, "[Stdin] "); custom_concat(cl_msg, tmp4); custom_concat(cl_msg, " client(i) conectat(i):");
                server_log(LOG_INFO, cl_msg);
                for (int i = 0; i < g_client_count; i++) {
                    char lc_msg[MAX_LOG_SIZE]; char tmp5[32];
                    custom_strcpy(lc_msg, "[Stdin]   "); custom_itoa(i + 1, tmp5); custom_concat(lc_msg, tmp5); custom_concat(lc_msg, ". ");
                    custom_concat(lc_msg, g_clients[i].ip); custom_concat(lc_msg, " (fd="); custom_itoa(g_clients[i].fd, tmp5); custom_concat(lc_msg, tmp5); custom_concat(lc_msg, ")");
                    server_log(LOG_INFO, lc_msg);
                }
            }
            pthread_mutex_unlock(&g_clients_mutex);

        } else if (strcmp(line, "shutdown") == 0) {
            server_log(LOG_INFO, "[Stdin] Shutdown initiat de la consola.");
            g_running = 0;
            break;

        } else {
            char unk_msg[MAX_LOG_SIZE]; custom_strcpy(unk_msg, "[Stdin] Comanda necunoscuta: '"); custom_concat(unk_msg, line); custom_concat(unk_msg, "'. Tastati 'help'.");
            server_log(LOG_WARN, unk_msg);
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
    char msg[MAX_LOG_SIZE]; char tmp[32]; custom_itoa(port, tmp);
    custom_strcpy(msg, "  Port TCP     : "); custom_concat(msg, tmp);
    server_log(LOG_INFO, msg);
    char msg2[MAX_LOG_SIZE]; char tmp2[32]; custom_itoa(ADMIN_PORT, tmp2);
    custom_strcpy(msg2, "  Port UDP(Adm): "); custom_concat(msg2, tmp2);
    server_log(LOG_INFO, msg2);
        char lm[MAX_LOG_SIZE]; custom_strcpy(lm, "  Log file     : "); custom_concat(lm, LOG_FILE); server_log(LOG_INFO, lm);
    server_log(LOG_INFO, "================================================");

    // pornim threadurile de servicii (Nivel B: exclusiv pthread pentru sincronizare)
    pthread_t admin_tid, proc_tid, inotify_tid, stdin_tid;

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

                    char acc_msg[MAX_LOG_SIZE]; char tmp6[32]; custom_itoa(cfd, tmp6);
                    custom_strcpy(acc_msg, "[Server] Client acceptat: "); custom_concat(acc_msg, ip); custom_concat(acc_msg, " fd="); custom_concat(acc_msg, tmp6);
                    server_log(LOG_INFO, acc_msg);
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
                char dec_msg[MAX_LOG_SIZE]; char tmp7[32]; custom_itoa(poll_fds[i].fd, tmp7);
                custom_strcpy(dec_msg, "[Server] Client deconectat fd="); custom_concat(dec_msg, tmp7);
                server_log(LOG_INFO, dec_msg);
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

    char om[MAX_LOG_SIZE]; char tmp99[32]; custom_itoa((int)g_running, tmp99); custom_strcpy(om, "[Server] Oprire curata (g_running="); custom_concat(om, tmp99); custom_concat(om, ")."); server_log(LOG_INFO, om);
    close(server_fd);
    close(g_work_pipe[0]);
    close(g_work_pipe[1]);
    curl_global_cleanup();
    if (g_log_file) { fclose(g_log_file); g_log_file = NULL; }
    return 0;
}
