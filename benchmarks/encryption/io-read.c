/* Read-only O_DIRECT diagnostic: one outstanding request per worker. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static pthread_barrier_t gate;
static const char *path;
static size_t block;
static int workers, random_access;
static double duration;
struct worker { int id, error; uint64_t count; double io_seconds; };
static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static void *run(void *arg) {
    struct worker *w = arg;
    int fd = open(path, O_RDONLY | O_DIRECT);
    struct stat st;
    void *buf = NULL;
    if (fd < 0 || fstat(fd, &st) || posix_memalign(&buf, 4096, block)) {
        perror("read setup"); exit(2);
    }
    uint64_t slots = st.st_size / block, position = w->id, seed = 253 + w->id;
    pthread_barrier_wait(&gate);
    double until = now() + duration;
    do {
        if (random_access) {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            position = seed % slots;
        }
        double start = now();
        ssize_t n = pread(fd, buf, block, (position % slots) * block);
        w->io_seconds += now() - start;
        if (n != (ssize_t)block) { w->error = 1; break; }
        ++w->count;
        if (!random_access) position += workers;
    } while (now() < until);
    close(fd); free(buf);
    return NULL;
}
int main(int argc, char **argv) {
    if (argc != 6) return 1;
    path = argv[1]; block = strtoull(argv[2], NULL, 10);
    workers = atoi(argv[3]); duration = atof(argv[4]); random_access = atoi(argv[5]);
    if (!block || block % 4096 || workers < 1 || workers > 32 || duration <= 0) return 1;
    pthread_t threads[32]; struct worker w[32] = {0};
    pthread_barrier_init(&gate, NULL, workers + 1);
    struct rusage before, after;
    getrusage(RUSAGE_SELF, &before);
    for (int i=0; i<workers; ++i) {
        w[i].id = i;
        if (pthread_create(&threads[i], NULL, run, &w[i])) return 2;
    }
    double start = now();
    pthread_barrier_wait(&gate);
    uint64_t operations = 0; double io_seconds = 0;
    for (int i=0; i<workers; ++i) {
        pthread_join(threads[i], NULL);
        if (w[i].error) return 3;
        operations += w[i].count; io_seconds += w[i].io_seconds;
    }
    double seconds = now() - start;
    getrusage(RUSAGE_SELF, &after);
    printf("{\"block_bytes\":%zu,\"workers\":%d,\"random\":%d,"
           "\"seconds\":%.6f,\"operations\":%llu,\"mib_per_second\":%.3f,"
           "\"mean_io_us\":%.3f,\"voluntary_context_switches\":%ld}\n",
           block, workers, random_access, seconds, (unsigned long long)operations,
           operations * (double)block / 1048576 / seconds,
           io_seconds * 1e6 / operations, after.ru_nvcsw - before.ru_nvcsw);
    return 0;
}
