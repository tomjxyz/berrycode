/* berrycode - the purple "Munching..." animation shown while the model works.
 * Runs in its own thread so it keeps moving while we wait on the network. */
#include "bc.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *MESSAGES[] = {
    "Munching", "Juicing", "Ripening", "Picking berries", "Squishing",
    "Fermenting", "Jamming", "Foraging", "Plucking", "Bramble-wrangling",
    "Simmering compote", "Blending a smoothie", "Sweetening", "Growing on the vine",
};
#define NMSG (sizeof MESSAGES / sizeof *MESSAGES)

/* a berry that grows and shrinks */
static const char *FRAMES[] = { ".", "o", "O", "@", "@", "O", "o", "." };
#define NFRAMES (sizeof FRAMES / sizeof *FRAMES)

static pthread_t       th;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int             running;      /* thread alive */
static volatile int    stop_req;
static const char     *msg;
static int             drawn;        /* width of what's on the line now */
static struct timespec t0;

static double elapsed(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)(ts.tv_sec - t0.tv_sec) + (double)(ts.tv_nsec - t0.tv_nsec) / 1e9;
}

static void clear_line(void)
{
    if (!drawn) return;
    printf("\r\033[K");
    fflush(stdout);
    drawn = 0;
}

static void draw(unsigned tick)
{
    int secs = (int)elapsed();
    int dots = (int)(tick / 3 % 4);                 /* "", ".", "..", "..." */
    char line[96];
    int n = snprintf(line, sizeof line, "%s %s%.*s", FRAMES[tick % NFRAMES], msg, dots, "...");
    char t[24] = "";
    int tn = secs >= 2 ? snprintf(t, sizeof t, " (%ds)", secs) : 0;
    /* pad the dots to a fixed width so the timer doesn't wobble */
    int pad = 3 - dots;
    /* must fit on one row (never the last column), or "\r" can't get back to its start */
    int avail = term_cols() - 1;
    if (n + pad + tn > avail) { tn = 0; t[0] = 0; }
    if (n + pad > avail) { pad = 0; if (n > avail) n = avail; }
    printf("\r%s%.*s%s%*s%s%s%s\033[K", g_cfg.color ? "\033[1;35m" : "", n, line, C_RESET, pad, "", C_DIM, t, C_RESET);
    fflush(stdout);
    drawn = 1;
}

static void *spin(void *arg)
{
    (void)arg;
    unsigned tick = 0;
    struct timespec nap = { 0, 120 * 1000 * 1000 };
    while (!stop_req) {
        pthread_mutex_lock(&mu);
        if (!stop_req) draw(tick++);
        pthread_mutex_unlock(&mu);
        /* change the message every ~10 s so long waits stay lively */
        if (tick % 84 == 0) msg = MESSAGES[(unsigned)rand() % NMSG];
        nanosleep(&nap, NULL);
    }
    return NULL;
}

void spinner_start(void)
{
    if (running || !g_cfg.animation || !isatty(1)) return;
    msg = MESSAGES[(unsigned)rand() % NMSG];
    clock_gettime(CLOCK_MONOTONIC, &t0);
    stop_req = 0;
    drawn = 0;
    /* keep Ctrl-C going to the main thread (it aborts the request there) */
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    if (pthread_create(&th, NULL, spin, NULL) == 0) running = 1;
    pthread_sigmask(SIG_SETMASK, &old, NULL);
}

void spinner_stop(void)
{
    if (!running) return;
    pthread_mutex_lock(&mu);
    stop_req = 1;
    clear_line();
    pthread_mutex_unlock(&mu);
    pthread_join(th, NULL);
    running = 0;
}

int spinner_active(void) { return running; }
