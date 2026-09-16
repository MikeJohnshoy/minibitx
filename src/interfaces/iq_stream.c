// iq_stream.c - see iq_stream.h for scope and rationale.
//
// Wire format, deliberately as simple as this codebase's other two I/Q
// paths (hpsdr_p1.c, usb_gadget.c) are protocol-heavy: no discovery, no
// start/stop handshake, no control registers - a client just sends any
// UDP datagram (content ignored) to IQ_STREAM_PORT to subscribe, and
// keeps receiving data packets as long as it re-sends that "still here"
// datagram at least once every SUBSCRIBER_TIMEOUT_SEC seconds (a lapsed
// subscriber is simply dropped - no explicit unsubscribe needed, so a
// client that crashes or is closed without cleanup doesn't leak a slot
// forever). Up to MAX_SUBSCRIBERS clients can be subscribed at once,
// each getting an identical copy of the same stream - this runs
// completely independently of hpsdr_p1.c's own single-client I/Q link,
// so a client here (tools/rigctl_panel.py's spectrum display) can be
// open at the same time as WSJT-X/Thetis on the HPSDR link without
// either disturbing the other.
//
// Data packet layout (all multi-byte fields big-endian/network order,
// same convention hpsdr_p1.c uses):
//
//   Offset  Size  Field
//   0       4     magic: 'I' 'Q' 'S' '1'
//   4       4     seq (uint32) - increments once per packet; a client
//                 can use gaps in this to notice dropped packets, but
//                 nothing here acts on it
//   8       4     n_samples (uint32) - always SAMPLES_PER_PACKET today,
//                 sent explicitly so a client doesn't have to hardcode it
//   12      4*n   n_samples pairs of (int16 I, int16 Q), each scaled so
//                 full-scale baseband amplitude reads close to full
//                 int16 range - plenty of dynamic range for a spectrum
//                 display, which only needs relative levels, not the
//                 calibrated absolute amplitude hpsdr_p1.c's SDR-app
//                 audience wants (see its own 24-bit choice)
//
// Real-time safety follows hpsdr_p1.c's own pattern exactly: the audio
// thread (iq_stream_send(), the producer) only ever appends to a
// lock-free single-producer/single-consumer ring buffer and returns
// immediately; a dedicated pacer thread is the sole consumer, draining
// it and sending one packet every SAMPLES_PER_PACKET/96000 seconds. The
// separate subscriber list (who to send to) is touched only by the
// listener thread (writer) and the pacer thread (reader) - both
// ordinary-priority threads, never the SCHED_FIFO audio thread - so a
// plain mutex around it is fine here; it would not be fine around the
// sample ring buffer, which is why that part stays lock-free (see
// hpsdr_p1.c's own comment on the priority-inversion trap a mutex caused
// there when one side was the real-time audio thread).

#include "iq_stream.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define SAMPLES_PER_PACKET 128
#define PACKET_INTERVAL_NS 1333333L // 128 samples / 96000 Hz, exact enough (1.3333...ms)
#define MAX_SUBSCRIBERS 4
#define SUBSCRIBER_TIMEOUT_SEC 5

// Same ring-buffer sizing rationale as hpsdr_p1.c's IQ_QUEUE_CAP: a
// power of two comfortably larger than one bursty audio-thread block,
// so it smooths rather than ever running dry or full in normal use.
#define IQ_QUEUE_CAP 4096
#define IQ_QUEUE_MASK (IQ_QUEUE_CAP - 1)

static int stream_sock = -1;
static volatile int running = 0;
static uint32_t tx_seq = 0;
static pthread_t listener_thread_id;
static pthread_t pacer_thread_id;

// --- Subscriber list -----------------------------------------------------
// Touched only by iq_stream_listener_thread() (adds/refreshes) and
// iq_stream_pacer_thread() (reads to know where to send, prunes expired
// entries) - never by the real-time audio thread, so a plain mutex is
// fine (see file header).
struct subscriber {
  int in_use;
  struct sockaddr_in addr;
  time_t last_seen; // CLOCK_MONOTONIC seconds
};
static struct subscriber subscribers[MAX_SUBSCRIBERS];
static pthread_mutex_t subscribers_lock = PTHREAD_MUTEX_INITIALIZER;

static time_t monotonic_now_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec;
}

// Records/refreshes sender as a subscriber. Reuses an existing entry for
// the same address:port if present (the common case - a client re-
// subscribing to stay alive), otherwise claims a free slot, otherwise
// (all MAX_SUBSCRIBERS slots full with distinct clients) replaces the
// stalest one - a new subscriber is more likely wanted than silently
// refusing it, and a genuinely-still-active client will simply resubscribe
// and reclaim a slot the next time one frees up.
static void subscriber_touch(const struct sockaddr_in *addr) {
  pthread_mutex_lock(&subscribers_lock);

  int free_slot = -1;
  int oldest_slot = 0;
  time_t oldest_seen = 0;

  for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
    if (subscribers[i].in_use && subscribers[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
        subscribers[i].addr.sin_port == addr->sin_port) {
      subscribers[i].last_seen = monotonic_now_sec();
      pthread_mutex_unlock(&subscribers_lock);
      return;
    }
    if (!subscribers[i].in_use && free_slot < 0)
      free_slot = i;
    if (i == 0 || subscribers[i].last_seen < oldest_seen) {
      oldest_seen = subscribers[i].last_seen;
      oldest_slot = i;
    }
  }

  int slot = (free_slot >= 0) ? free_slot : oldest_slot;
  int was_new_client = !subscribers[slot].in_use || subscribers[slot].addr.sin_addr.s_addr !=
                                                         addr->sin_addr.s_addr;
  subscribers[slot].in_use = 1;
  subscribers[slot].addr = *addr;
  subscribers[slot].last_seen = monotonic_now_sec();

  pthread_mutex_unlock(&subscribers_lock);

  if (was_new_client) {
    printf("iq_stream: subscriber added %s:%d\n", inet_ntoa(addr->sin_addr), ntohs(addr->sin_port));
  }
}

// --- Lock-free SPSC IQ ring buffer, same design as hpsdr_p1.c's -------
static double q_i[IQ_QUEUE_CAP];
static double q_q[IQ_QUEUE_CAP];
static atomic_uint q_head = 0;
static atomic_uint q_tail = 0;

static double pkt_buf_i[SAMPLES_PER_PACKET];
static double pkt_buf_q[SAMPLES_PER_PACKET];

void iq_stream_send(const double *i_samples, const double *q_samples, int n) {
  if (stream_sock < 0)
    return;

  // Cheap, racy "is anyone even subscribed" check, same spirit as
  // hpsdr_send_iq()'s client_active guard - avoids paying for the ring
  // buffer writes below when nobody's listening. A stale read here (a
  // subscriber added/expired the instant after this check) costs at
  // most one audio block's worth of samples either dropped or queued
  // for nothing, never a correctness problem.
  int any_active = 0;
  pthread_mutex_lock(&subscribers_lock);
  for (int i = 0; i < MAX_SUBSCRIBERS; i++)
    any_active |= subscribers[i].in_use;
  pthread_mutex_unlock(&subscribers_lock);
  if (!any_active)
    return;

  unsigned head = atomic_load_explicit(&q_head, memory_order_relaxed);
  unsigned tail = atomic_load_explicit(&q_tail, memory_order_acquire);

  for (int k = 0; k < n; k++) {
    unsigned next_head = (head + 1) & IQ_QUEUE_MASK;
    if (next_head == tail) {
      // Ring full - drop this sample rather than block the audio
      // thread (see hpsdr_p1.c's identical tradeoff).
      continue;
    }
    q_i[head] = i_samples[k];
    q_q[head] = q_samples[k];
    head = next_head;
  }

  atomic_store_explicit(&q_head, head, memory_order_release);
}

static void build_and_send_packet(void) {
  uint8_t pkt[12 + SAMPLES_PER_PACKET * 4];

  pkt[0] = 'I';
  pkt[1] = 'Q';
  pkt[2] = 'S';
  pkt[3] = '1';
  pkt[4] = (tx_seq >> 24) & 0xFF;
  pkt[5] = (tx_seq >> 16) & 0xFF;
  pkt[6] = (tx_seq >> 8) & 0xFF;
  pkt[7] = tx_seq & 0xFF;
  tx_seq++;
  pkt[8] = (SAMPLES_PER_PACKET >> 24) & 0xFF;
  pkt[9] = (SAMPLES_PER_PACKET >> 16) & 0xFF;
  pkt[10] = (SAMPLES_PER_PACKET >> 8) & 0xFF;
  pkt[11] = SAMPLES_PER_PACKET & 0xFF;

  for (int s = 0; s < SAMPLES_PER_PACKET; s++) {
    uint8_t *sp = pkt + 12 + s * 4;

    // Scaled so full-scale baseband amplitude reads close to full int16
    // range - see file header on why this doesn't need hpsdr_p1.c's
    // calibrated 24-bit scale factor.
    int32_t i_val = (int32_t)(pkt_buf_i[s] * 32767.0);
    if (i_val > 32767)
      i_val = 32767;
    if (i_val < -32768)
      i_val = -32768;

    int32_t q_val = (int32_t)(pkt_buf_q[s] * 32767.0);
    if (q_val > 32767)
      q_val = 32767;
    if (q_val < -32768)
      q_val = -32768;

    sp[0] = (i_val >> 8) & 0xFF;
    sp[1] = i_val & 0xFF;
    sp[2] = (q_val >> 8) & 0xFF;
    sp[3] = q_val & 0xFF;
  }

  pthread_mutex_lock(&subscribers_lock);
  time_t now = monotonic_now_sec();
  for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
    if (!subscribers[i].in_use)
      continue;
    if (now - subscribers[i].last_seen > SUBSCRIBER_TIMEOUT_SEC) {
      printf("iq_stream: subscriber %s:%d timed out\n", inet_ntoa(subscribers[i].addr.sin_addr),
             ntohs(subscribers[i].addr.sin_port));
      subscribers[i].in_use = 0;
      continue;
    }
    sendto(stream_sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&subscribers[i].addr,
           sizeof(subscribers[i].addr));
  }
  pthread_mutex_unlock(&subscribers_lock);
}

static void *iq_stream_pacer_thread(void *arg) {
  (void)arg;
  struct timespec next;
  clock_gettime(CLOCK_MONOTONIC, &next);

  while (running) {
    next.tv_nsec += PACKET_INTERVAL_NS;
    while (next.tv_nsec >= 1000000000L) {
      next.tv_nsec -= 1000000000L;
      next.tv_sec++;
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

    if (stream_sock < 0)
      continue;

    unsigned head = atomic_load_explicit(&q_head, memory_order_acquire);
    unsigned tail = atomic_load_explicit(&q_tail, memory_order_relaxed);
    unsigned available = (head - tail) & IQ_QUEUE_MASK;

    if (available >= SAMPLES_PER_PACKET) {
      for (int s = 0; s < SAMPLES_PER_PACKET; s++) {
        pkt_buf_i[s] = q_i[tail];
        pkt_buf_q[s] = q_q[tail];
        tail = (tail + 1) & IQ_QUEUE_MASK;
      }
      atomic_store_explicit(&q_tail, tail, memory_order_release);
      build_and_send_packet();
    }
    // else: not enough queued yet (e.g. just after a fresh subscribe,
    // before the ring has filled) - skip this tick, try again next.
  }
  return NULL;
}

static void *iq_stream_listener_thread(void *arg) {
  (void)arg;
  uint8_t buf[64]; // content is never inspected - any datagram subscribes
  struct sockaddr_in sender;
  socklen_t sender_len;

  while (running) {
    sender_len = sizeof(sender);
    int n = recvfrom(stream_sock, buf, sizeof(buf), 0, (struct sockaddr *)&sender, &sender_len);
    if (n >= 0)
      subscriber_touch(&sender);
    // n < 0 here is almost always the SO_RCVTIMEO timeout below expiring
    // with nothing received - not an error worth logging, just a chance
    // for the running flag to be re-checked.
  }
  return NULL;
}

int iq_stream_init(void) {
  stream_sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (stream_sock < 0)
    return -1;

  int optval = 1;
  setsockopt(stream_sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(IQ_STREAM_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(stream_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(stream_sock);
    stream_sock = -1;
    return -1;
  }

  // Same purpose as hpsdr_p1.c's identical setsockopt: without a receive
  // timeout, recvfrom() in the listener thread blocks forever, and this
  // thread would never notice `running` go to 0 during shutdown.
  struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
  setsockopt(stream_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  running = 1;
  return 0;
}

void iq_stream_poll(void) {
  static int started = 0;
  if (!started && running) {
    pthread_create(&listener_thread_id, NULL, iq_stream_listener_thread, NULL);
    pthread_create(&pacer_thread_id, NULL, iq_stream_pacer_thread, NULL);
    started = 1;
  }
}

void iq_stream_stop(void) {
  running = 0;

  if (stream_sock >= 0) {
    close(stream_sock);
    stream_sock = -1;
  }
}
