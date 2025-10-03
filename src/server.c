// server.c
// Multi-threaded TCP server using epoll + thread pool
// Build: gcc -O2 -pthread -o server server.c

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#define LISTEN_PORT 9000
#define LISTEN_BACKLOG 512
#define MAX_EVENTS 4096
#define MAX_WORKERS 8
#define TASK_QUEUE_SIZE 65536
#define READ_BUF_SIZE 4096

// Simple task: server main thread reads bytes and enqueues tasks (fd + copy of buffer)
typedef struct {
    int fd;
    size_t len;
    char *buf; // allocated buffer
} task_t;

// Task queue (ring buffer)
typedef struct {
    task_t *items;
    size_t capacity;
    atomic_size_t head;
    atomic_size_t tail;
    pthread_mutex_t mtx;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} task_queue_t;

static int server_fd = -1;
static int epoll_fd = -1;
static volatile sig_atomic_t stop_server = 0;
static task_queue_t tq;
static pthread_t workers[MAX_WORKERS];
static int worker_count = MAX_WORKERS;
static atomic_int active_connections = 0;

// ------------ Utility ------------

static void perror_exit(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// ------------ Task Queue ------------

static void task_queue_init(task_queue_t *q, size_t capacity) {
    q->items = calloc(capacity, sizeof(task_t));
    if (!q->items) perror_exit("calloc");
    q->capacity = capacity;
    atomic_store(&q->head, 0);
    atomic_store(&q->tail, 0);
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

static void task_queue_destroy(task_queue_t *q) {
    free(q->items);
    pthread_mutex_destroy(&q->mtx);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

static int task_queue_push(task_queue_t *q, task_t t) {
    pthread_mutex_lock(&q->mtx);
    while ((atomic_load(&q->tail) + 1) % q->capacity == atomic_load(&q->head)) {
        // full
        pthread_cond_wait(&q->not_full, &q->mtx);
    }
    size_t idx = atomic_load(&q->tail);
    q->items[idx] = t;
    atomic_store(&q->tail, (idx + 1) % q->capacity);
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mtx);
    return 0;
}

static int task_queue_pop(task_queue_t *q, task_t *out) {
    pthread_mutex_lock(&q->mtx);
    while (atomic_load(&q->head) == atomic_load(&q->tail) && !stop_server) {
        pthread_cond_wait(&q->not_empty, &q->mtx);
    }
    if (stop_server && atomic_load(&q->head) == atomic_load(&q->tail)) {
        pthread_mutex_unlock(&q->mtx);
        return -1; // shutdown
    }
    size_t idx = atomic_load(&q->head);
    *out = q->items[idx];
    atomic_store(&q->head, (idx + 1) % q->capacity);
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mtx);
    return 0;
}

// ------------ Worker ------------

static void msleep(long ms) {
    struct timespec ts = { .tv_sec = ms/1000, .tv_nsec = (ms%1000)*1000000 };
    nanosleep(&ts, NULL);
}

// Attempt to send all bytes; makes some retries on EAGAIN but does NOT block indefinitely.
// For production, implement per-fd write buffers + EPOLLOUT rearm.
static ssize_t send_all_nonblocking(int fd, const char *buf, size_t len, int max_attempts) {
    size_t sent = 0;
    int attempts = 0;
    while (sent < len && attempts < max_attempts) {
        ssize_t s = send(fd, buf + sent, len - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (s > 0) {
            sent += (size_t)s;
            continue;
        } else if (s == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            attempts++;
            // small backoff
            msleep(2);
            continue;
        } else if (s == -1 && errno == EINTR) {
            continue;
        } else {
            // other error (peer closed?), return -1
            return -1;
        }
    }
    return (ssize_t)sent;
}

static void *worker_fn(void *arg) {
    (void)arg;
    while (!stop_server) {
        task_t t;
        if (task_queue_pop(&tq, &t) == -1) break; // shutdown
        // Process task - example: echo server + simple processing
        // You may replace this with parsing request/protocol logic.
        if (t.fd >= 0 && t.len > 0 && t.buf) {
            // Example processing: append timestamp and echo back
            char timebuf[64];
            time_t now = time(NULL);
            struct tm tm;
            localtime_r(&now, &tm);
            strftime(timebuf, sizeof(timebuf), " [%Y-%m-%d %H:%M:%S]\n", &tm);

            size_t outlen = t.len + strlen(timebuf);
            char *out = malloc(outlen);
            if (out) {
                memcpy(out, t.buf, t.len);
                memcpy(out + t.len, timebuf, strlen(timebuf));
                ssize_t s = send_all_nonblocking(t.fd, out, outlen, 1000);
                free(out);
                if (s < 0) {
                    // error writing - close socket from worker side
                    close(t.fd);
                    atomic_fetch_sub(&active_connections, 1);
                }
            }
        }
        // free task buffer
        if (t.buf) free(t.buf);
    }
    return NULL;
}

// ------------ Epoll main loop ------------

static void handle_new_connection(int server_fd, int epoll_fd) {
    while (1) {
        struct sockaddr_in in_addr;
        socklen_t in_len = sizeof(in_addr);
        int infd = accept(server_fd, (struct sockaddr *)&in_addr, &in_len);
        if (infd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // all accepted
                break;
            } else {
                perror("accept");
                break;
            }
        }
        // set non-blocking
        if (set_nonblocking(infd) == -1) {
            perror("fcntl");
            close(infd);
            continue;
        }
        // add to epoll
        struct epoll_event ev;
        ev.data.fd = infd;
        ev.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, infd, &ev) == -1) {
            perror("epoll_ctl: add");
            close(infd);
            continue;
        }
        atomic_fetch_add(&active_connections, 1);
        // Optionally print remote info
        char ipbuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &in_addr.sin_addr, ipbuf, sizeof(ipbuf));
        // printf("Accepted connection from %s:%d (fd=%d)\n", ipbuf, ntohs(in_addr.sin_port), infd);
    }
}

static void close_connection(int fd) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
    atomic_fetch_sub(&active_connections, 1);
}

static void run_event_loop() {
    struct epoll_event *events = calloc(MAX_EVENTS, sizeof(struct epoll_event));
    if (!events) perror_exit("calloc events");

    while (!stop_server) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000); // 1s timeout
        if (n == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        } else if (n == 0) {
            // timeout - continue to check stop flag
            continue;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            if (fd == server_fd) {
                // new connection
                handle_new_connection(server_fd, epoll_fd);
                continue;
            }

            if (ev & (EPOLLHUP | EPOLLRDHUP)) {
                close_connection(fd);
                continue;
            }

            if (ev & EPOLLIN) {
                // read all available data
                while (1) {
                    char *buf = malloc(READ_BUF_SIZE);
                    if (!buf) { perror("malloc"); break; }
                    ssize_t count = recv(fd, buf, READ_BUF_SIZE, 0);
                    if (count == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            free(buf);
                            break; // no more data now
                        } else if (errno == EINTR) {
                            free(buf);
                            continue;
                        } else {
                            // read error - close
                            free(buf);
                            close_connection(fd);
                            break;
                        }
                    } else if (count == 0) {
                        // client closed
                        free(buf);
                        close_connection(fd);
                        break;
                    } else {
                        // got data -> enqueue task with a copy
                        task_t t;
                        t.fd = fd;
                        t.len = (size_t)count;
                        t.buf = buf; // ownership transferred to queue
                        task_queue_push(&tq, t);
                        // continue reading in case more data already available
                        // (we loop to drain socket)
                        continue;
                    }
                }
            }

            // Note: we do NOT handle EPOLLOUT here. For robust large-writes, add write buffers and EPOLLOUT.
        }
    }

    free(events);
}

// ------------ Setup & Shutdown ------------

static void stop_handler(int signum) {
    (void)signum;
    stop_server = 1;
    // wake workers
    pthread_mutex_lock(&tq.mtx);
    pthread_cond_broadcast(&tq.not_empty);
    pthread_mutex_unlock(&tq.mtx);
}

int main(int argc, char **argv) {
    int port = LISTEN_PORT;
    if (argc >= 2) port = atoi(argv[1]);
    if (argc >= 3) worker_count = atoi(argv[2]);
    if (worker_count <= 0) worker_count = MAX_WORKERS;

    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);

    // Initialize task queue
    task_queue_init(&tq, TASK_QUEUE_SIZE);

    // Create server socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) perror_exit("socket");

    // allow reuse
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror_exit("setsockopt");
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) perror_exit("bind");
    if (listen(server_fd, LISTEN_BACKLOG) == -1) perror_exit("listen");

    if (set_nonblocking(server_fd) == -1) perror_exit("set_nonblocking");

    // Create epoll
    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) perror_exit("epoll_create1");

    struct epoll_event event;
    event.data.fd = server_fd;
    event.events = EPOLLIN;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) == -1) perror_exit("epoll_ctl: server_fd");

    // start worker threads
    for (int i = 0; i < worker_count; ++i) {
        if (pthread_create(&workers[i], NULL, worker_fn, NULL) != 0) {
            perror_exit("pthread_create");
        }
    }

    printf("Server listening on port %d with %d workers. PID=%d\n", port, worker_count, getpid());
    run_event_loop();

    // Shutdown sequence
    printf("Shutting down server...\n");
    // close server fd so epoll stops accepting new
    if (server_fd != -1) close(server_fd);

    // wake workers to exit
    pthread_mutex_lock(&tq.mtx);
    pthread_cond_broadcast(&tq.not_empty);
    pthread_mutex_unlock(&tq.mtx);

    // join workers
    for (int i = 0; i < worker_count; ++i) {
        pthread_join(workers[i], NULL);
    }

    // close remaining fds? epoll closed by OS on exit
    task_queue_destroy(&tq);

    if (epoll_fd != -1) close(epoll_fd);

    printf("Active connections at shutdown: %d\n", (int)atomic_load(&active_connections));
    return 0;
}
