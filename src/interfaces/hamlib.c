// hamlib.c - see hamlib.h for scope.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "hamlib.h"
#include "cw.h"
#include "rx_audio.h"

extern int freq_hdr;    // current frequency, Hz - see radio.h
extern int in_tx;       // 0 = RX, 1 = TX - see radio.h
extern void radio_tune_to(uint32_t f);
extern void radio_set_tx(int tx_on);

static int listen_fd = -1;
static volatile int running = 0;
static pthread_t accept_thread;

// Cosmetic-only "current mode" state - minibitx has no onboard demod
// (the SDR app does all mode selection/filtering in software), so M/m
// just let a client believe its mode selection stuck, without minibitx
// acting on it in any way.
static char current_mode[32] = "USB";
static int current_passband = 2400;

#define LINE_MAX_LEN 256

static void send_line(int fd, const char *s)
{
    // Best-effort - a client that vanished mid-command isn't fatal,
    // MSG_NOSIGNAL keeps a dead peer from raising SIGPIPE.
    send(fd, s, strlen(s), MSG_NOSIGNAL);
}

static void send_rprt(int fd, int code)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "RPRT %d\n", code);
    send_line(fd, buf);
}

// Handles one already-newline-stripped command line for one connection.
// Returns 0 to keep the connection open, -1 to close it (q/Q).
static int handle_line(int fd, char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n')) {
        line[--len] = '\0';
    }
    if (len == 0) return 0;

    // Extended-response prefix ('+') - we don't implement the labeled
    // echo format some clients can request, just accept and ignore the
    // prefix so a client that defaults to sending it still gets a
    // normal reply instead of an unknown-command error.
    char *cmd = line;
    if (cmd[0] == '+') cmd++;
    while (*cmd == ' ' || *cmd == '\t') cmd++;

    if (strcmp(cmd, "q") == 0 || strcmp(cmd, "Q") == 0 ||
        strcmp(cmd, "quit") == 0) {
        printf("rigctl: %s -> closing connection\n", cmd);
        return -1;
    }

    if (cmd[0] == 'f' && (cmd[1] == '\0' || cmd[1] == ' ')) {
        // get_freq
        char buf[32];
        snprintf(buf, sizeof(buf), "%d\n", freq_hdr);
        send_line(fd, buf);
        printf("rigctl: f -> %d Hz\n", freq_hdr);
        return 0;
    }

    if (cmd[0] == 'F' && (cmd[1] == '\0' || cmd[1] == ' ')) {
        // set_freq <hz>
        long f = strtol(cmd + 1, NULL, 10);
        if (f <= 0) {
            send_rprt(fd, -1);
            printf("rigctl: F%s -> invalid frequency, ignored\n", cmd + 1);
            return 0;
        }
        radio_tune_to((uint32_t)f);
        send_rprt(fd, 0);
        printf("rigctl: F %ld -> tuned to %ld Hz\n", f, f);
        return 0;
    }

    if (cmd[0] == 't' && (cmd[1] == '\0' || cmd[1] == ' ')) {
        // get_ptt
        char buf[8];
        snprintf(buf, sizeof(buf), "%d\n", in_tx ? 1 : 0);
        send_line(fd, buf);
        printf("rigctl: t -> %s\n", in_tx ? "TX" : "RX");
        return 0;
    }

    if (cmd[0] == 'T' && (cmd[1] == '\0' || cmd[1] == ' ')) {
        // set_ptt <0|1|2|3> - minibitx has one TX state, no separate
        // mic/data distinction, so anything nonzero means TX.
        long v = strtol(cmd + 1, NULL, 10);
        int tx_on = (v != 0);
        if (cw_tx_active()) {
            send_rprt(fd, 0);
            printf("rigctl: T %ld -> ignored, local CW key holds TX\n", v);
            return 0;
       }
        radio_set_tx(tx_on);
        send_rprt(fd, 0);
        printf("rigctl: T %ld -> %s\n", v, tx_on ? "TX on" : "TX off");
        return 0;
    }

    if (cmd[0] == 'm' && (cmd[1] == '\0' || cmd[1] == ' ')) {
        // get_mode - two lines: mode, then passband
        char buf[64];
        snprintf(buf, sizeof(buf), "%s\n%d\n", current_mode, current_passband);
        send_line(fd, buf);
        printf("rigctl: m -> %s %d\n", current_mode, current_passband);
        return 0;
    }

    if (cmd[0] == 'M' && (cmd[1] == '\0' || cmd[1] == ' ')) {
        // set_mode <mode> <passband> - cosmetic only, see comment above
        char mode[32] = "";
        int passband = current_passband;
        sscanf(cmd + 1, "%31s %d", mode, &passband);
        if (mode[0]) {
            strncpy(current_mode, mode, sizeof(current_mode) - 1);
            current_mode[sizeof(current_mode) - 1] = '\0';
        }
        current_passband = passband;
        send_rprt(fd, 0);
        printf("rigctl: M %s %d -> ok (cosmetic, not applied)\n",
               current_mode, current_passband);
        return 0;
    }

    if (cmd[0] == 'l' && (cmd[1] == '\0' || cmd[1] == ' ')) {
        // get_level <name> - AF (audio/volume) and STRENGTH (S-meter) are
        // backed by something real; everything else in the hamlib level
        // set (RF, SQL, preamp, attenuator, ...) has no minibitx
        // equivalent, same spirit as dump_state's empty preamp/attenuator
        // lists above. Unlike u/U NARROW, STRENGTH IS a real Hamlib
        // RIG_LEVEL (see dump_state's comment below), so this replies in
        // the standard convention real Hamlib clients expect - see
        // rx_audio_get_strength_db()'s comment for what the number means
        // and doesn't mean.
        char level_name[32] = "";
        sscanf(cmd + 1, "%31s", level_name);
        if (strcmp(level_name, "AF") == 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.6f\n", rx_audio_get_volume() / 100.0);
            send_line(fd, buf);
            printf("rigctl: l AF -> %d%%\n", rx_audio_get_volume());
        } else if (strcmp(level_name, "STRENGTH") == 0) {
            int db = rx_audio_get_strength_db();
            char buf[16];
            snprintf(buf, sizeof(buf), "%d\n", db);
            send_line(fd, buf);
            printf("rigctl: l STRENGTH -> %d (dB relative to S9)\n", db);
        } else {
            send_rprt(fd, -1);
            printf("rigctl: l %s -> unsupported level\n", level_name);
        }
        return 0;
    }

    if (cmd[0] == 'L' && cmd[1] == ' ') {
        // set_level <name> <value 0.0-1.0> - STRENGTH deliberately has no
        // case here: it's read-only, same as a real rig's S-meter, so
        // "L STRENGTH ..." falls through to the unsupported-level reply
        // below, matching real Hamlib rigs (nothing implements set_level
        // for RIG_LEVEL_STRENGTH).
        char level_name[32] = "";
        double val = 0.0;
        if (sscanf(cmd + 1, "%31s %lf", level_name, &val) == 2 &&
            strcmp(level_name, "AF") == 0) {
            int percent = (int)(val * 100.0 + 0.5);
            rx_audio_set_volume(percent);
            send_rprt(fd, 0);
            printf("rigctl: L AF %.6f -> volume %d%%\n", val, percent);
        } else {
            send_rprt(fd, -1);
            printf("rigctl: L %s -> unsupported level or bad args\n", cmd + 1);
        }
        return 0;
    }

    if (cmd[0] == 'u' && (cmd[1] == '\0' || cmd[1] == ' ')) {
        // get_func <name> - only NARROW (the post-demod "single signal"
        // selectivity filter, rx_audio.c stage 3) is backed by anything
        // real, same "everything else has no minibitx equivalent" spirit
        // as l/L AF above. NARROW isn't a name real Hamlib ships in its
        // own function table - this rigctld subset only ever talks to
        // tools/rigctl_panel.py, not stock rigctl, so there's no
        // compatibility reason to hunt for a closer standard name.
        char func_name[32] = "";
        sscanf(cmd + 1, "%31s", func_name);
        if (strcmp(func_name, "NARROW") == 0) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d\n", rx_audio_get_narrow_filter());
            send_line(fd, buf);
            printf("rigctl: u NARROW -> %s\n",
                   rx_audio_get_narrow_filter() ? "on" : "off");
        } else {
            send_rprt(fd, -1);
            printf("rigctl: u %s -> unsupported function\n", func_name);
        }
        return 0;
    }

    if (cmd[0] == 'U' && cmd[1] == ' ') {
        // set_func <name> <0|1>
        char func_name[32] = "";
        int val = 0;
        if (sscanf(cmd + 1, "%31s %d", func_name, &val) == 2 &&
            strcmp(func_name, "NARROW") == 0) {
            rx_audio_set_narrow_filter(val != 0);
            send_rprt(fd, 0);
            printf("rigctl: U NARROW %d -> narrow filter %s\n", val,
                   val ? "on" : "off");
        } else {
            send_rprt(fd, -1);
            printf("rigctl: U %s -> unsupported function or bad args\n", cmd + 1);
        }
        return 0;
    }

    if (cmd[0] == 'v' && (cmd[1] == '\0' || cmd[1] == ' ')) {
        // get_vfo - minibitx has only one VFO, always report it
        send_line(fd, "VFOA\n");
        printf("rigctl: v -> VFOA\n");
        return 0;
    }
    if (cmd[0] == 'V' && cmd[1] == ' ') {
        // set_vfo <vfo> - minibitx has only one VFO; accept and report success
        // regardless of the requested name, since there's nothing else to switch to.
        send_rprt(fd, 0);
        printf("rigctl: V %s -> ok (single VFO)\n", cmd + 2);
        return 0;
    }

    if (strcmp(cmd, "chk_vfo") == 0 || strcmp(cmd, "\\chk_vfo") == 0) {
        // Single-VFO radio - report "not in VFO mode" so callers send
        // plain f/F/t/T without needing a VFO argument.
        send_line(fd, "0\n");
        printf("rigctl: chk_vfo -> 0\n");
        return 0;
    }

    if (strcmp(cmd, "dump_state") == 0 || strcmp(cmd, "\\dump_state") == 0) {
        // Minimal, spec-shaped dump_state (format confirmed against
        // Hamlib's own rigctl_parse.c dump_state() implementation).
        // Deliberately advertises no capabilities minibitx doesn't
        // actually have - no RIT/XIT/IF shift, no preamp/attenuator, no
        // onboard filters, since the SDR app does all of that in
        // software - and an empty TX range, since minibitx has no TX
        // audio path yet even though radio_set_tx() can key PTT.
        // has_get_level advertises RIG_LEVEL_AF (1<<3 = 0x8) OR'd with
        // RIG_LEVEL_STRENGTH (1<<30 = 0x40000000, per hamlib's rig.h) =
        // 0x40000008 - the two real levels, backed by
        // rx_audio_set_volume()/rx_audio_get_volume() via l/L AF and
        // rx_audio_get_strength_db() via l STRENGTH above. has_set_level
        // stays 0x8 (AF only): STRENGTH is deliberately absent from it,
        // same as on a real rig - S-meter readings are get_level-only,
        // there's no "set the S-meter" operation (see L's own comment).
        // has_get_func/has_set_func stay 0x0 despite u/U NARROW above
        // actually doing something: NARROW isn't a real RIG_FUNC bit (see
        // u/U's own comment), and this dump_state is only ever read by
        // tools/rigctl_panel.py, which doesn't gate anything on it - real
        // rigctl/Hamlib clients would have no bit to advertise it under.
        send_line(fd, "0\n");                        // protocol version
        send_line(fd, "1\n");                        // rig model (1 = RIG_MODEL_DUMMY)
        send_line(fd, "2\n");                         // ITU region (best-effort default)
        send_line(fd, "0 30000000 0x1ff -1 -1 0x1 0x0\n"); // RX range: 0-30MHz, all modes, RX-only, VFO A
        send_line(fd, "0 0 0 0 0 0 0\n");             // RX range list terminator
        send_line(fd, "0 0 0 0 0 0 0\n");             // empty TX range list
        send_line(fd, "0x1ff 1\n");                   // one tuning step: 1 Hz, all modes
        send_line(fd, "0 0\n");                       // tuning step list terminator
        send_line(fd, "0 0\n");                       // empty filter list
        send_line(fd, "0\n");                         // max_rit
        send_line(fd, "0\n");                         // max_xit
        send_line(fd, "0\n");                         // max_ifshift
        send_line(fd, "0\n");                         // announces
        send_line(fd, "\n");                          // preamp list (empty)
        send_line(fd, "\n");                          // attenuator list (empty)
        send_line(fd, "0x0\n");                       // has_get_func
        send_line(fd, "0x0\n");                       // has_set_func
        send_line(fd, "0x40000008\n");                // has_get_level (RIG_LEVEL_AF | RIG_LEVEL_STRENGTH)
        send_line(fd, "0x8\n");                       // has_set_level (RIG_LEVEL_AF only)
        send_line(fd, "0x0\n");                       // has_get_parm
        send_line(fd, "0x0\n");                       // has_set_parm
        printf("rigctl: dump_state -> sent\n");
        return 0;
    }

    // Unknown command - reply cleanly instead of hanging the client.
    send_rprt(fd, -1);
    printf("rigctl: %s -> unknown command\n", cmd);
    return 0;
}

static void *client_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;

    char buf[LINE_MAX_LEN];
    size_t buf_len = 0;

    while (running) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) break;   // disconnected or error

        if (c == '\n') {
            buf[buf_len] = '\0';
            if (handle_line(fd, buf) < 0) break;
            buf_len = 0;
        } else if (buf_len + 1 < sizeof(buf)) {
            buf[buf_len++] = c;
        }
        // else: line too long - silently drop extra bytes until '\n'
    }

    close(fd);
    printf("hamlib: client disconnected\n");
    return NULL;
}

static void *accept_thread_fn(void *arg)
{
    (void)arg;
    while (running) {
        struct sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        int fd = accept(listen_fd, (struct sockaddr *)&client_addr, &len);
        if (fd < 0) {
            if (!running) break;
            continue;
        }

        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        printf("hamlib: client connected from %s:%d\n",
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, (void *)(intptr_t)fd) == 0) {
            pthread_detach(tid);
        } else {
            close(fd);
        }
    }
    return NULL;
}

int hamlib_init(int port)
{
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        printf("hamlib: socket() failed: %s\n", strerror(errno));
        return -1;
    }

    int optval = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("hamlib: bind() to port %d failed: %s\n", port, strerror(errno));
        close(listen_fd);
        listen_fd = -1;
        return -1;
    }

    if (listen(listen_fd, 4) < 0) {
        printf("hamlib: listen() failed: %s\n", strerror(errno));
        close(listen_fd);
        listen_fd = -1;
        return -1;
    }

    running = 1;
    if (pthread_create(&accept_thread, NULL, accept_thread_fn, NULL) != 0) {
        printf("hamlib: failed to start accept thread\n");
        running = 0;
        close(listen_fd);
        listen_fd = -1;
        return -1;
    }
    pthread_detach(accept_thread);
    printf("init: Hamlib/rigctld listening on TCP %d\n", port);
    return 0;
}

void hamlib_stop(void)
{
    running = 0;
    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }
}
