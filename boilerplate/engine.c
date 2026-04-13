#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define SOCKET_PATH "/tmp/jackfruit.sock"
#define STACK_SIZE (1024 * 1024)
#define REQ_MAX 1024
#define LOG_CHUNK 512
#define LOG_QUEUE_CAP 256
#define DEFAULT_SOFT_MIB 40
#define DEFAULT_HARD_MIB 64
#define STOP_GRACE_SEC 2
#define MAX_CMD_ARGS 64

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_EXITED
} container_state_t;

typedef enum {
    REASON_UNKNOWN = 0,
    REASON_NORMAL_EXIT,
    REASON_MANUALLY_STOPPED,
    REASON_HARD_LIMIT_KILLED,
    REASON_SIGNALED
} final_reason_t;

typedef struct container container_t;

typedef struct {
    container_t *container;
    size_t len;
    char data[LOG_CHUNK];
} log_entry_t;

typedef struct {
    log_entry_t entries[LOG_QUEUE_CAP];
    int head;
    int tail;
    int count;
    int active_producers;
    int shutdown;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} log_queue_t;

typedef struct {
    char id[64];
    char rootfs[PATH_MAX];
    int argc;
    char *argv[MAX_CMD_ARGS];
    int soft_mib;
    int hard_mib;
    int has_nice;
    int nice_value;
} start_request_t;

typedef struct {
    char id[64];
    char rootfs[PATH_MAX];
    int argc;
    char **argv;
    int has_nice;
    int nice_value;
    int log_write_fd;
} clone_child_cfg_t;

struct container {
    char id[64];
    pid_t pid;
    time_t start_time;
    container_state_t state;
    final_reason_t reason;
    char rootfs[PATH_MAX];
    char command[256];
    int soft_mib;
    int hard_mib;
    int has_nice;
    int nice_value;
    char log_path[PATH_MAX];
    int log_fd;
    int log_read_fd;
    int exit_code;
    int term_signal;
    int final_status;
    int stop_requested;
    int producer_started;
    int producer_joined;
    pthread_t producer_tid;
    pthread_mutex_t lock;
    pthread_cond_t exited_cv;
    void *child_stack;
    clone_child_cfg_t *child_cfg;
    container_t *next;
};

static log_queue_t g_log_queue;
static pthread_t g_log_consumer_tid;
static pthread_t g_reaper_tid;
static pthread_mutex_t g_meta_lock = PTHREAD_MUTEX_INITIALIZER;
static container_t *g_containers = NULL;
static volatile sig_atomic_t g_shutdown = 0;
static int g_server_fd = -1;
static char g_base_rootfs[PATH_MAX];
static char g_base_parent[PATH_MAX];

static void free_start_request(start_request_t *req)
{
    int i;
    for (i = 0; i < req->argc; i++) {
        free(req->argv[i]);
        req->argv[i] = NULL;
    }
    req->argc = 0;
}

static void free_child_cfg(clone_child_cfg_t *cfg)
{
    int i;

    if (cfg == NULL) {
        return;
    }
    if (cfg->argv != NULL) {
        for (i = 0; i < cfg->argc; i++) {
            free(cfg->argv[i]);
        }
        free(cfg->argv);
    }
    free(cfg);
}

static int has_live_containers(void)
{
    container_t *cur;
    int alive = 0;

    pthread_mutex_lock(&g_meta_lock);
    cur = g_containers;
    while (cur != NULL) {
        pthread_mutex_lock(&cur->lock);
        if (cur->state != CONTAINER_EXITED) {
            alive = 1;
            pthread_mutex_unlock(&cur->lock);
            break;
        }
        pthread_mutex_unlock(&cur->lock);
        cur = cur->next;
    }
    pthread_mutex_unlock(&g_meta_lock);

    return alive;
}

static int canonicalize_rootfs_path(const char *input, char *resolved, size_t cap)
{
    char candidate[PATH_MAX];
    char tmp[PATH_MAX];
    struct stat st;

    if (input == NULL || input[0] == '\0') {
        return -1;
    }

    if (input[0] == '/') {
        snprintf(candidate, sizeof(candidate), "%s", input);
    } else {
        size_t base_len = strlen(g_base_parent);
        size_t input_len = strlen(input);
        if (base_len + 1 + input_len + 1 > sizeof(candidate)) {
            return -1;
        }
        memcpy(candidate, g_base_parent, base_len);
        candidate[base_len] = '/';
        memcpy(candidate + base_len + 1, input, input_len + 1);
    }

    if (realpath(candidate, tmp) == NULL) {
        if (input[0] == '/') {
            return -1;
        }
        if (realpath(input, tmp) == NULL) {
            return -1;
        }
    }

    {
        size_t n = strlen(g_base_parent);
        if (strncmp(tmp, g_base_parent, n) != 0 ||
            !(g_base_parent[1] == '\0' || tmp[n] == '\0' || tmp[n] == '/')) {
            return -1;
        }
    }

    if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return -1;
    }

    snprintf(resolved, cap, "%s", tmp);
    return 0;
}

static void join_command_for_metadata(container_t *c, const start_request_t *req)
{
    int i;
    size_t used = 0;

    c->command[0] = '\0';
    for (i = 0; i < req->argc; i++) {
        int n = snprintf(c->command + used,
                         sizeof(c->command) - used,
                         "%s%s",
                         i == 0 ? "" : " ",
                         req->argv[i]);
        if (n < 0) {
            break;
        }
        if ((size_t)n >= sizeof(c->command) - used) {
            used = sizeof(c->command) - 1;
            break;
        }
        used += (size_t)n;
    }
}

static const char *state_to_string(container_state_t st)
{
    switch (st) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_EXITED:
        return "exited";
    }
    return "unknown";
}

static const char *reason_to_string(final_reason_t reason)
{
    switch (reason) {
    case REASON_NORMAL_EXIT:
        return "normal_exit";
    case REASON_MANUALLY_STOPPED:
        return "manually_stopped";
    case REASON_HARD_LIMIT_KILLED:
        return "hard_limit_killed";
    case REASON_SIGNALED:
        return "signaled";
    default:
        return "unknown";
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> [--soft-mib N] [--hard-mib N] [--nice N] <command> [args...]\n"
            "  %s run <id> <container-rootfs> [--soft-mib N] [--hard-mib N] [--nice N] <command> [args...]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog,
            prog,
            prog,
            prog,
            prog,
            prog);
}

static int safe_write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static void monitor_register_container(container_t *c)
{
    int fd = open("/dev/container_monitor", O_RDWR | O_CLOEXEC);
    struct monitor_request req;

    if (fd < 0) {
        return;
    }

    memset(&req, 0, sizeof(req));
    req.pid = c->pid;
    req.soft_limit_bytes = (unsigned long)c->soft_mib * 1024UL * 1024UL;
    req.hard_limit_bytes = (unsigned long)c->hard_mib * 1024UL * 1024UL;
    snprintf(req.container_id, sizeof(req.container_id), "%s", c->id);
    (void)ioctl(fd, MONITOR_REGISTER, &req);
    close(fd);
}

static void monitor_unregister_container(const container_t *c)
{
    int fd = open("/dev/container_monitor", O_RDWR | O_CLOEXEC);
    struct monitor_request req;

    if (fd < 0) {
        return;
    }

    memset(&req, 0, sizeof(req));
    req.pid = c->pid;
    snprintf(req.container_id, sizeof(req.container_id), "%s", c->id);
    (void)ioctl(fd, MONITOR_UNREGISTER, &req);
    close(fd);
}

static container_t *find_container_by_id_locked(const char *id)
{
    container_t *cur = g_containers;
    while (cur != NULL) {
        if (strcmp(cur->id, id) == 0) {
            return cur;
        }
        cur = cur->next;
    }
    return NULL;
}

static container_t *find_container_by_pid_locked(pid_t pid)
{
    container_t *cur = g_containers;
    while (cur != NULL) {
        if (cur->pid == pid) {
            return cur;
        }
        cur = cur->next;
    }
    return NULL;
}

static int parse_start_tokens(char **tokens, int ntok, start_request_t *out)
{
    int i;

    if (ntok < 4) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    snprintf(out->id, sizeof(out->id), "%s", tokens[1]);
    snprintf(out->rootfs, sizeof(out->rootfs), "%s", tokens[2]);
    out->soft_mib = DEFAULT_SOFT_MIB;
    out->hard_mib = DEFAULT_HARD_MIB;

    for (i = 3; i < ntok; i++) {
        if (strcmp(tokens[i], "--soft-mib") == 0) {
            if (i + 1 >= ntok) {
                free_start_request(out);
                return -1;
            }
            out->soft_mib = atoi(tokens[++i]);
            continue;
        }
        if (strcmp(tokens[i], "--hard-mib") == 0) {
            if (i + 1 >= ntok) {
                free_start_request(out);
                return -1;
            }
            out->hard_mib = atoi(tokens[++i]);
            continue;
        }
        if (strcmp(tokens[i], "--nice") == 0) {
            if (i + 1 >= ntok) {
                free_start_request(out);
                return -1;
            }
            out->has_nice = 1;
            out->nice_value = atoi(tokens[++i]);
            continue;
        }

        if (out->argc >= MAX_CMD_ARGS - 1) {
            free_start_request(out);
            return -1;
        }
        out->argv[out->argc] = strdup(tokens[i]);
        if (out->argv[out->argc] == NULL) {
            free_start_request(out);
            return -1;
        }
        out->argc++;
    }

    if (out->argc == 0) {
        free_start_request(out);
        return -1;
    }

    if (out->soft_mib <= 0 || out->hard_mib <= 0 || out->soft_mib > out->hard_mib) {
        free_start_request(out);
        return -1;
    }

    if (out->has_nice && (out->nice_value < -20 || out->nice_value > 19)) {
        free_start_request(out);
        return -1;
    }

    return 0;
}

static int child_main(void *arg)
{
    clone_child_cfg_t *cfg = (clone_child_cfg_t *)arg;

    close(STDIN_FILENO);
    dup2(cfg->log_write_fd, STDOUT_FILENO);
    dup2(cfg->log_write_fd, STDERR_FILENO);
    if (cfg->log_write_fd > STDERR_FILENO) {
        close(cfg->log_write_fd);
    }

    if (cfg->has_nice) {
        (void)setpriority(PRIO_PROCESS, 0, cfg->nice_value);
    }

    if (sethostname(cfg->id, strlen(cfg->id)) != 0) {
        perror("sethostname");
    }

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        perror("mount private");
    }

    if (chroot(cfg->rootfs) != 0) {
        perror("chroot");
        _exit(127);
    }
    if (chdir("/") != 0) {
        perror("chdir");
        _exit(127);
    }

    (void)mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount /proc");
    }

    execvp(cfg->argv[0], cfg->argv);
    fprintf(stderr,
            "execvp failed for '%s': %s\n",
            cfg->argv[0] ? cfg->argv[0] : "<null>",
            strerror(errno));
    perror("execvp");
    _exit(127);
}

static void *log_producer_thread(void *arg)
{
    container_t *c = (container_t *)arg;
    char buf[LOG_CHUNK];

    while (1) {
        ssize_t n = read(c->log_read_fd, buf, sizeof(buf));
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        pthread_mutex_lock(&g_log_queue.lock);
        while (g_log_queue.count == LOG_QUEUE_CAP) {
            pthread_cond_wait(&g_log_queue.not_full, &g_log_queue.lock);
        }

        g_log_queue.entries[g_log_queue.tail].container = c;
        g_log_queue.entries[g_log_queue.tail].len = (size_t)n;
        memcpy(g_log_queue.entries[g_log_queue.tail].data, buf, (size_t)n);
        g_log_queue.tail = (g_log_queue.tail + 1) % LOG_QUEUE_CAP;
        g_log_queue.count++;

        pthread_cond_signal(&g_log_queue.not_empty);
        pthread_mutex_unlock(&g_log_queue.lock);
    }

    close(c->log_read_fd);
    c->log_read_fd = -1;

    pthread_mutex_lock(&g_log_queue.lock);
    g_log_queue.active_producers--;
    pthread_cond_broadcast(&g_log_queue.not_empty);
    pthread_mutex_unlock(&g_log_queue.lock);

    return NULL;
}

static void *log_consumer_thread(void *arg)
{
    (void)arg;

    while (1) {
        log_entry_t entry;

        pthread_mutex_lock(&g_log_queue.lock);
        while (g_log_queue.count == 0 &&
               !(g_log_queue.shutdown && g_log_queue.active_producers == 0)) {
            pthread_cond_wait(&g_log_queue.not_empty, &g_log_queue.lock);
        }

        if (g_log_queue.count == 0 && g_log_queue.shutdown && g_log_queue.active_producers == 0) {
            pthread_mutex_unlock(&g_log_queue.lock);
            break;
        }

        entry = g_log_queue.entries[g_log_queue.head];
        g_log_queue.head = (g_log_queue.head + 1) % LOG_QUEUE_CAP;
        g_log_queue.count--;
        pthread_cond_signal(&g_log_queue.not_full);
        pthread_mutex_unlock(&g_log_queue.lock);

        if (entry.container != NULL && entry.container->log_fd >= 0) {
            (void)safe_write_all(entry.container->log_fd, entry.data, entry.len);
        }
    }

    return NULL;
}

typedef struct {
    container_t *container;
} stop_force_arg_t;

static void *force_kill_thread(void *arg)
{
    stop_force_arg_t *ctx = (stop_force_arg_t *)arg;
    container_t *c = ctx->container;
    free(ctx);

    sleep(STOP_GRACE_SEC);

    pthread_mutex_lock(&c->lock);
    if (c->state != CONTAINER_EXITED && c->stop_requested) {
        kill(c->pid, SIGKILL);
    }
    pthread_mutex_unlock(&c->lock);
    return NULL;
}

static int request_stop(container_t *c)
{
    pthread_t tid;
    stop_force_arg_t *ctx;

    pthread_mutex_lock(&c->lock);
    if (c->state == CONTAINER_EXITED) {
        pthread_mutex_unlock(&c->lock);
        return 0;
    }
    c->stop_requested = 1;
    c->reason = REASON_MANUALLY_STOPPED;
    pthread_mutex_unlock(&c->lock);

    if (kill(c->pid, SIGTERM) != 0 && errno != ESRCH) {
        return -1;
    }

    ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return -1;
    }
    ctx->container = c;
    if (pthread_create(&tid, NULL, force_kill_thread, ctx) == 0) {
        pthread_detach(tid);
    } else {
        free(ctx);
    }
    return 0;
}

static int start_container(const start_request_t *req, container_t **out)
{
    container_t *c;
    int pipefd[2];
    int flags;
    pid_t pid;
    char *stack;
    char resolved_rootfs[PATH_MAX];

    if (canonicalize_rootfs_path(req->rootfs, resolved_rootfs, sizeof(resolved_rootfs)) != 0) {
        return -9;
    }

    pthread_mutex_lock(&g_meta_lock);
    {
        container_t *it = g_containers;
        while (it != NULL) {
            if (strcmp(it->id, req->id) == 0 && it->state != CONTAINER_EXITED) {
                pthread_mutex_unlock(&g_meta_lock);
                return -1;
            }
            if (strcmp(it->rootfs, resolved_rootfs) == 0 && it->state != CONTAINER_EXITED) {
                pthread_mutex_unlock(&g_meta_lock);
                return -2;
            }
            it = it->next;
        }
    }
    pthread_mutex_unlock(&g_meta_lock);

    c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return -3;
    }
    pthread_mutex_init(&c->lock, NULL);
    pthread_cond_init(&c->exited_cv, NULL);

    snprintf(c->id, sizeof(c->id), "%s", req->id);
    snprintf(c->rootfs, sizeof(c->rootfs), "%s", resolved_rootfs);
    join_command_for_metadata(c, req);
    snprintf(c->log_path, sizeof(c->log_path), "logs/%s.log", req->id);
    c->soft_mib = req->soft_mib;
    c->hard_mib = req->hard_mib;
    c->has_nice = req->has_nice;
    c->nice_value = req->nice_value;
    c->state = CONTAINER_STARTING;
    c->reason = REASON_UNKNOWN;
    c->exit_code = -1;
    c->term_signal = 0;
    c->final_status = -1;
    c->log_fd = -1;
    c->log_read_fd = -1;

    (void)mkdir("logs", 0755);
    c->log_fd = open(c->log_path, O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
    if (c->log_fd < 0) {
        free(c);
        return -4;
    }

    if (pipe(pipefd) != 0) {
        close(c->log_fd);
        free(c);
        return -5;
    }

    c->child_cfg = calloc(1, sizeof(*c->child_cfg));
    if (c->child_cfg == NULL) {
        close(pipefd[0]);
        close(pipefd[1]);
        close(c->log_fd);
        free(c);
        return -6;
    }
    snprintf(c->child_cfg->id, sizeof(c->child_cfg->id), "%s", c->id);
    snprintf(c->child_cfg->rootfs, sizeof(c->child_cfg->rootfs), "%s", c->rootfs);
    c->child_cfg->argc = req->argc;
    c->child_cfg->argv = calloc((size_t)req->argc + 1, sizeof(char *));
    if (c->child_cfg->argv == NULL) {
        close(pipefd[0]);
        close(pipefd[1]);
        close(c->log_fd);
        free_child_cfg(c->child_cfg);
        free(c);
        return -6;
    }
    for (int idx = 0; idx < req->argc; idx++) {
        c->child_cfg->argv[idx] = strdup(req->argv[idx]);
        if (c->child_cfg->argv[idx] == NULL) {
            close(pipefd[0]);
            close(pipefd[1]);
            close(c->log_fd);
            free_child_cfg(c->child_cfg);
            free(c);
            return -6;
        }
    }
    c->child_cfg->argv[req->argc] = NULL;
    c->child_cfg->has_nice = c->has_nice;
    c->child_cfg->nice_value = c->nice_value;
    c->child_cfg->log_write_fd = pipefd[1];

    stack = malloc(STACK_SIZE);
    if (stack == NULL) {
        close(pipefd[0]);
        close(pipefd[1]);
        close(c->log_fd);
        free_child_cfg(c->child_cfg);
        free(c);
        return -7;
    }
    c->child_stack = stack;

    flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid = clone(child_main, stack + STACK_SIZE, flags, c->child_cfg);
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        close(c->log_fd);
        free_child_cfg(c->child_cfg);
        free(c->child_stack);
        free(c);
        return -8;
    }

    close(pipefd[1]);
    c->log_read_fd = pipefd[0];
    c->pid = pid;
    c->start_time = time(NULL);
    c->state = CONTAINER_RUNNING;

    pthread_mutex_lock(&g_log_queue.lock);
    g_log_queue.active_producers++;
    pthread_mutex_unlock(&g_log_queue.lock);

    if (pthread_create(&c->producer_tid, NULL, log_producer_thread, c) == 0) {
        c->producer_started = 1;
    } else {
        close(c->log_read_fd);
        c->log_read_fd = -1;
        pthread_mutex_lock(&g_log_queue.lock);
        g_log_queue.active_producers--;
        pthread_mutex_unlock(&g_log_queue.lock);
    }

    pthread_mutex_lock(&g_meta_lock);
    c->next = g_containers;
    g_containers = c;
    pthread_mutex_unlock(&g_meta_lock);

    monitor_register_container(c);

    *out = c;
    return 0;
}

static void mark_container_exit(container_t *c, int status)
{
    int stop_requested;

    pthread_mutex_lock(&c->lock);
    stop_requested = c->stop_requested;
    c->state = CONTAINER_EXITED;

    if (WIFEXITED(status)) {
        c->exit_code = WEXITSTATUS(status);
        c->final_status = c->exit_code;
        if (stop_requested) {
            c->reason = REASON_MANUALLY_STOPPED;
        } else {
            c->reason = REASON_NORMAL_EXIT;
        }
    } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        c->term_signal = sig;
        c->final_status = 128 + sig;
        if (stop_requested) {
            c->reason = REASON_MANUALLY_STOPPED;
        } else if (sig == SIGKILL) {
            c->reason = REASON_HARD_LIMIT_KILLED;
        } else {
            c->reason = REASON_SIGNALED;
        }
    }

    pthread_cond_broadcast(&c->exited_cv);
    pthread_mutex_unlock(&c->lock);
}

static void *reaper_thread(void *arg)
{
    sigset_t *set = (sigset_t *)arg;

    while (1) {
        int sig = 0;
        if (sigwait(set, &sig) != 0) {
            continue;
        }
        if (sig != SIGCHLD) {
            continue;
        }

        while (1) {
            pid_t pid;
            int status = 0;
            container_t *c = NULL;

            pid = waitpid(-1, &status, WNOHANG);
            if (pid <= 0) {
                break;
            }

            pthread_mutex_lock(&g_meta_lock);
            c = find_container_by_pid_locked(pid);
            pthread_mutex_unlock(&g_meta_lock);
            if (c == NULL) {
                continue;
            }

            monitor_unregister_container(c);
            mark_container_exit(c, status);

            if (c->producer_started && !c->producer_joined) {
                pthread_join(c->producer_tid, NULL);
                c->producer_joined = 1;
            }
        }

        if (g_shutdown && !has_live_containers()) {
            break;
        }
    }
    return NULL;
}

static void server_signal_handler(int sig)
{
    (void)sig;
    g_shutdown = 1;
    if (g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
    }
}

static void append_ps_line(char *buf, size_t cap, const char *line)
{
    size_t used = strlen(buf);
    if (used < cap - 1) {
        snprintf(buf + used, cap - used, "%s", line);
    }
}

static void handle_ps(int client_fd)
{
    char out[8192];
    container_t *cur;
    struct tm tmv;

    out[0] = '\0';
    append_ps_line(out, sizeof(out), "id pid state reason soft_mib hard_mib nice start_ts exit signal stop_requested log\n");

    pthread_mutex_lock(&g_meta_lock);
    cur = g_containers;
    while (cur != NULL) {
        char line[2048];
        char nice_buf[16];
        long ts = 0;
        char tbuf[64] = "-";
        int exit_code;
        int term_signal;
        int stop_requested;
        int has_nice;
        int nice_value;
        container_state_t state;
        final_reason_t reason;

        pthread_mutex_lock(&cur->lock);
        exit_code = cur->exit_code;
        term_signal = cur->term_signal;
        stop_requested = cur->stop_requested;
        has_nice = cur->has_nice;
        nice_value = cur->nice_value;
        state = cur->state;
        reason = cur->reason;
        ts = (long)cur->start_time;
        pthread_mutex_unlock(&cur->lock);

        if (has_nice) {
            snprintf(nice_buf, sizeof(nice_buf), "%d", nice_value);
        } else {
            snprintf(nice_buf, sizeof(nice_buf), "-");
        }

        localtime_r(&cur->start_time, &tmv);
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", &tmv);

        snprintf(line,
                 sizeof(line),
                 "%.63s %d %.15s %.31s %d %d %s %s %d %d %d %.1023s\n",
                 cur->id,
                 cur->pid,
                 state_to_string(state),
                 reason_to_string(reason),
                 cur->soft_mib,
                 cur->hard_mib,
                 nice_buf,
                 ts ? tbuf : "-",
                 exit_code,
                 term_signal,
                 stop_requested,
                 cur->log_path);
        append_ps_line(out, sizeof(out), line);
        cur = cur->next;
    }
    pthread_mutex_unlock(&g_meta_lock);

    (void)safe_write_all(client_fd, out, strlen(out));
}

static void handle_logs(int client_fd, const char *id)
{
    container_t *c;
    int fd;
    char buf[1024];

    pthread_mutex_lock(&g_meta_lock);
    c = find_container_by_id_locked(id);
    pthread_mutex_unlock(&g_meta_lock);

    if (c == NULL) {
        (void)safe_write_all(client_fd, "ERR unknown container\n", 22);
        return;
    }

    fd = open(c->log_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        (void)safe_write_all(client_fd, "ERR log unavailable\n", 18);
        return;
    }

    while (1) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (safe_write_all(client_fd, buf, (size_t)n) != 0) {
            break;
        }
    }
    close(fd);
}

static void handle_stop(int client_fd, const char *id)
{
    container_t *c;

    pthread_mutex_lock(&g_meta_lock);
    c = find_container_by_id_locked(id);
    pthread_mutex_unlock(&g_meta_lock);

    if (c == NULL) {
        (void)safe_write_all(client_fd, "ERR unknown container\n", 22);
        return;
    }

    if (request_stop(c) != 0) {
        (void)safe_write_all(client_fd, "ERR stop failed\n", 16);
        return;
    }

    (void)safe_write_all(client_fd, "OK stop requested\n", 18);
}

static void handle_start_or_run(int client_fd, char **tokens, int ntok, int is_run)
{
    start_request_t req;
    container_t *c = NULL;
    char response[256];
    int rc;

    if (parse_start_tokens(tokens, ntok, &req) != 0) {
        (void)safe_write_all(client_fd, "ERR invalid arguments\n", 22);
        return;
    }

    rc = start_container(&req, &c);
    if (rc != 0 || c == NULL) {
        free_start_request(&req);
        if (rc == -1) {
            (void)safe_write_all(client_fd, "ERR duplicate container id\n", 27);
        } else if (rc == -2) {
            (void)safe_write_all(client_fd, "ERR rootfs already in use\n", 26);
        } else if (rc == -9) {
            (void)safe_write_all(client_fd, "ERR invalid container-rootfs\n", 29);
        } else {
            (void)safe_write_all(client_fd, "ERR unable to start\n", 20);
        }
        return;
    }

    free_start_request(&req);

    if (!is_run) {
        snprintf(response, sizeof(response), "OK started id=%s pid=%d\n", c->id, c->pid);
        (void)safe_write_all(client_fd, response, strlen(response));
        return;
    }

    /* run semantics: keep request open until container has fully exited. */
    pthread_mutex_lock(&c->lock);
    while (c->state != CONTAINER_EXITED) {
        pthread_cond_wait(&c->exited_cv, &c->lock);
    }
    snprintf(response,
             sizeof(response),
             "OK exit=%d reason=%s\n",
             c->final_status,
             reason_to_string(c->reason));
    pthread_mutex_unlock(&c->lock);
    (void)safe_write_all(client_fd, response, strlen(response));
}

static void *client_thread(void *arg)
{
    int client_fd = *(int *)arg;
    char req[REQ_MAX];
    char *tokens[128];
    int ntok = 0;
    char *save;
    char *tok;
    ssize_t n;

    free(arg);
    memset(req, 0, sizeof(req));
    n = read(client_fd, req, sizeof(req) - 1);
    if (n <= 0) {
        close(client_fd);
        return NULL;
    }

    tok = strtok_r(req, " \t\r\n", &save);
    while (tok != NULL && ntok < (int)(sizeof(tokens) / sizeof(tokens[0]))) {
        tokens[ntok++] = tok;
        tok = strtok_r(NULL, " \t\r\n", &save);
    }

    if (ntok == 0) {
        (void)safe_write_all(client_fd, "ERR empty command\n", 18);
        close(client_fd);
        return NULL;
    }

    if (strcmp(tokens[0], "start") == 0) {
        handle_start_or_run(client_fd, tokens, ntok, 0);
    } else if (strcmp(tokens[0], "run") == 0) {
        handle_start_or_run(client_fd, tokens, ntok, 1);
    } else if (strcmp(tokens[0], "ps") == 0) {
        handle_ps(client_fd);
    } else if (strcmp(tokens[0], "logs") == 0 && ntok == 2) {
        handle_logs(client_fd, tokens[1]);
    } else if (strcmp(tokens[0], "stop") == 0 && ntok == 2) {
        handle_stop(client_fd, tokens[1]);
    } else {
        (void)safe_write_all(client_fd, "ERR unknown command\n", 20);
    }

    close(client_fd);
    return NULL;
}

static int connect_supervisor_socket(void)
{
    int fd;
    struct sockaddr_un addr;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_PATH);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_stop_intent(const char *id)
{
    int fd;
    char cmd[128];
    char resp[256];
    ssize_t n;

    fd = connect_supervisor_socket();
    if (fd < 0) {
        return -1;
    }

    snprintf(cmd, sizeof(cmd), "stop %s", id);
    if (safe_write_all(fd, cmd, strlen(cmd)) != 0) {
        close(fd);
        return -1;
    }

    n = read(fd, resp, sizeof(resp));
    (void)n;
    close(fd);
    return 0;
}

static volatile sig_atomic_t g_client_signal = 0;

static void run_client_signal_handler(int sig)
{
    g_client_signal = sig;
}

static int run_client_wait_loop(int fd, const char *id)
{
    int stop_sent = 0;
    char buf[4096];
    char acc[8192];
    size_t acc_len = 0;
    int exit_status = 1;
    int final_seen = 0;

    acc[0] = '\0';

    while (1) {
        struct pollfd pfd;
        int pr;

        pfd.fd = fd;
        pfd.events = POLLIN;
        pr = poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (g_client_signal != 0 && !stop_sent) {
            (void)send_stop_intent(id);
            stop_sent = 1;
        }

        if (pr == 0) {
            continue;
        }

        if (pfd.revents & POLLIN) {
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            if (n <= 0) {
                break;
            }
            buf[n] = '\0';
            fputs(buf, stdout);

            if (acc_len + (size_t)n >= sizeof(acc)) {
                acc_len = 0;
                acc[0] = '\0';
            }
            memcpy(acc + acc_len, buf, (size_t)n + 1);
            acc_len += (size_t)n;

            {
                char *needle = strstr(acc, "OK exit=");
                if (needle != NULL) {
                    int code;
                    if (sscanf(needle, "OK exit=%d", &code) == 1) {
                        exit_status = code;
                        final_seen = 1;
                    }
                }
            }
        }

        if (pfd.revents & (POLLERR | POLLHUP)) {
            if (final_seen) {
                break;
            }
        }
    }

    return exit_status;
}

static int run_client_command(int argc, char **argv)
{
    int fd;
    char req[REQ_MAX];
    int i;
    struct sigaction sa;

    if (argc < 5) {
        usage(argv[0]);
        return 1;
    }

    g_client_signal = 0;

    fd = connect_supervisor_socket();
    if (fd < 0) {
        perror("connect");
        return 1;
    }

    req[0] = '\0';
    for (i = 1; i < argc; i++) {
        if (strlen(req) + strlen(argv[i]) + 2 >= sizeof(req)) {
            close(fd);
            return 1;
        }
        strcat(req, argv[i]);
        if (i + 1 < argc) {
            strcat(req, " ");
        }
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = run_client_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (safe_write_all(fd, req, strlen(req)) != 0) {
        close(fd);
        return 1;
    }

    i = run_client_wait_loop(fd, argv[2]);
    close(fd);
    return i;
}

static int send_simple_client_command(int argc, char **argv)
{
    int fd;
    char req[REQ_MAX];
    char buf[4096];
    int i;

    fd = connect_supervisor_socket();
    if (fd < 0) {
        perror("connect");
        return 1;
    }

    req[0] = '\0';
    for (i = 1; i < argc; i++) {
        if (strlen(req) + strlen(argv[i]) + 2 >= sizeof(req)) {
            close(fd);
            return 1;
        }
        strcat(req, argv[i]);
        if (i + 1 < argc) {
            strcat(req, " ");
        }
    }

    if (safe_write_all(fd, req, strlen(req)) != 0) {
        close(fd);
        return 1;
    }

    while (1) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        (void)safe_write_all(STDOUT_FILENO, buf, (size_t)n);
    }

    close(fd);
    return 0;
}

static int send_start_client_command(int argc, char **argv)
{
    int fd;
    char req[REQ_MAX];
    char ch;
    char response[512];
    size_t used = 0;
    int i;

    fd = connect_supervisor_socket();
    if (fd < 0) {
        perror("connect");
        return 1;
    }

    req[0] = '\0';
    for (i = 1; i < argc; i++) {
        if (strlen(req) + strlen(argv[i]) + 2 >= sizeof(req)) {
            close(fd);
            return 1;
        }
        strcat(req, argv[i]);
        if (i + 1 < argc) {
            strcat(req, " ");
        }
    }

    if (safe_write_all(fd, req, strlen(req)) != 0) {
        close(fd);
        return 1;
    }

    memset(response, 0, sizeof(response));
    while (used < sizeof(response) - 1) {
        ssize_t n = read(fd, &ch, 1);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return 1;
        }
        response[used++] = ch;
        if (ch == '\n') {
            break;
        }
    }

    if (used > 0) {
        (void)safe_write_all(STDOUT_FILENO, response, used);
    }

    close(fd);
    return 0;
}

static int run_supervisor(const char *base_rootfs)
{
    struct sockaddr_un addr;
    struct sigaction sa;
    sigset_t sigset;
    struct stat st;
    char *slash;

    if (realpath(base_rootfs, g_base_rootfs) == NULL) {
        perror("invalid base-rootfs");
        return 1;
    }
    if (stat(g_base_rootfs, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "base-rootfs must be a directory\n");
        return 1;
    }

    snprintf(g_base_parent, sizeof(g_base_parent), "%s", g_base_rootfs);
    slash = strrchr(g_base_parent, '/');
    if (slash == NULL || slash == g_base_parent) {
        snprintf(g_base_parent, sizeof(g_base_parent), "/");
    } else {
        *slash = '\0';
    }

    /* Block SIGCHLD before creating threads so only reaper consumes it via sigwait(). */
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGCHLD);
    pthread_sigmask(SIG_BLOCK, &sigset, NULL);

    memset(&g_log_queue, 0, sizeof(g_log_queue));
    pthread_mutex_init(&g_log_queue.lock, NULL);
    pthread_cond_init(&g_log_queue.not_empty, NULL);
    pthread_cond_init(&g_log_queue.not_full, NULL);

    if (pthread_create(&g_log_consumer_tid, NULL, log_consumer_thread, NULL) != 0) {
        return 1;
    }

    if (pthread_create(&g_reaper_tid, NULL, reaper_thread, &sigset) != 0) {
        g_shutdown = 1;
        return 1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = server_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    g_server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_server_fd < 0) {
        perror("socket");
        g_shutdown = 1;
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_PATH);
    unlink(SOCKET_PATH);

    if (bind(g_server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(g_server_fd);
        g_server_fd = -1;
        return 1;
    }

    if (listen(g_server_fd, 32) != 0) {
        perror("listen");
        close(g_server_fd);
        g_server_fd = -1;
        unlink(SOCKET_PATH);
        return 1;
    }

    printf("Supervisor listening on %s\n", SOCKET_PATH);
    printf("Base rootfs: %s\n", g_base_rootfs);

    while (!g_shutdown) {
        int client_fd;
        int *fd_ptr;
        pthread_t tid;

        client_fd = accept(g_server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (g_shutdown) {
                break;
            }
            continue;
        }

        fd_ptr = malloc(sizeof(*fd_ptr));
        if (fd_ptr == NULL) {
            close(client_fd);
            continue;
        }
        *fd_ptr = client_fd;

        if (pthread_create(&tid, NULL, client_thread, fd_ptr) == 0) {
            pthread_detach(tid);
        } else {
            free(fd_ptr);
            close(client_fd);
        }
    }

    pthread_mutex_lock(&g_meta_lock);
    {
        container_t *cur = g_containers;
        while (cur != NULL) {
            request_stop(cur);
            cur = cur->next;
        }
    }
    pthread_mutex_unlock(&g_meta_lock);

    sleep(STOP_GRACE_SEC + 1);
    pthread_kill(g_reaper_tid, SIGCHLD);
    pthread_join(g_reaper_tid, NULL);

    pthread_mutex_lock(&g_log_queue.lock);
    g_log_queue.shutdown = 1;
    pthread_cond_broadcast(&g_log_queue.not_empty);
    pthread_cond_broadcast(&g_log_queue.not_full);
    pthread_mutex_unlock(&g_log_queue.lock);
    pthread_join(g_log_consumer_tid, NULL);

    pthread_mutex_lock(&g_meta_lock);
    while (g_containers != NULL) {
        container_t *next = g_containers->next;
        if (g_containers->producer_started && !g_containers->producer_joined) {
            pthread_join(g_containers->producer_tid, NULL);
        }
        if (g_containers->log_fd >= 0) {
            close(g_containers->log_fd);
        }
        pthread_cond_destroy(&g_containers->exited_cv);
        pthread_mutex_destroy(&g_containers->lock);
        free_child_cfg(g_containers->child_cfg);
        free(g_containers->child_stack);
        free(g_containers);
        g_containers = next;
    }
    pthread_mutex_unlock(&g_meta_lock);

    if (g_server_fd >= 0) {
        close(g_server_fd);
    }
    unlink(SOCKET_PATH);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc != 3) {
            usage(argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0) {
        if (argc < 5) {
            usage(argv[0]);
            return 1;
        }
        return send_start_client_command(argc, argv);
    }
    if (strcmp(argv[1], "run") == 0) {
        return run_client_command(argc, argv);
    }
    if (strcmp(argv[1], "ps") == 0 && argc == 2) {
        return send_simple_client_command(argc, argv);
    }
    if (strcmp(argv[1], "logs") == 0 && argc == 3) {
        return send_simple_client_command(argc, argv);
    }
    if (strcmp(argv[1], "stop") == 0 && argc == 3) {
        return send_simple_client_command(argc, argv);
    }

    usage(argv[0]);
    return 1;
}
