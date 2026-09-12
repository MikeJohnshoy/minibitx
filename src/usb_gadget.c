/*
 * usb_gadget.c — see usb_gadget.h for scope and architecture.
 *
 * Ported near-verbatim from the UAC2 section of sbitx's hpsdr_p1.c
 * (Mike/KB2ML). No sBitx/GTK dependency in this file — only ALSA and
 * Linux configfs/sysfs — so the port was mechanical.
 */

#include "usb_gadget.h"

#include "cw.h"
#include <alsa/asoundlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

extern int freq_hdr; // current frequency, Hz - see radio.h
extern int in_tx;    // 0 = RX, 1 = TX - see radio.h
extern void radio_tune_to(uint32_t f);
extern void radio_set_tx(int tx_on);

/* ---------------------------------------------------------------------
 * Compile-time configuration
 * --------------------------------------------------------------------- */

// Root of the Linux USB gadget configfs hierarchy
#define UAC_GADGET_ROOT "/sys/kernel/config/usb_gadget/sbitx_iq"

// PCM parameters to match the UAC2 descriptor
#define UAC_RATE 48000
#define UAC_CHANNELS 2
#define UAC_SAMPLE_BYTES 3    // packed on-wire bytes (24-bit PCM)
#define UAC_PERIOD_FRAMES 512 // ALSA period size in frames
#define UAC_PERIODS 4         // number of periods in the ring buffer

// One frame = UAC_CHANNELS * UAC_SAMPLE_BYTES bytes
#define UAC_FRAME_BYTES (UAC_CHANNELS * UAC_SAMPLE_BYTES)

// Internal ring: hold up to UAC_PERIOD_FRAMES samples before each ALSA write
#define UAC_BUF_FRAMES UAC_PERIOD_FRAMES
static uint8_t uac_pcm_buf[UAC_BUF_FRAMES * UAC_FRAME_BYTES];

/* ---------------------------------------------------------------------
 * IQ handoff queue - see the "producer/consumer split" note above
 * uac_push_iq() below. Same lock-free SPSC ring design as hpsdr_p1.c's
 * IQ queue (that file's comments have the full derivation of why a
 * plain mutex isn't safe to share with a real-time producer thread) -
 * producer (uac_push_iq(), called from sound.c's SCHED_FIFO audio
 * thread) only ever writes uac_q_head; consumer (uac_writer_thread())
 * only ever writes uac_q_tail. Neither ever blocks on the other.
 * --------------------------------------------------------------------- */
#define UAC_QUEUE_CAP                                                                             \
  8192 // power of two; ~170ms at 48kHz -
       // generous slack against USB-side stalls
#define UAC_QUEUE_MASK (UAC_QUEUE_CAP - 1)

static double uac_q_i[UAC_QUEUE_CAP];
static double uac_q_q[UAC_QUEUE_CAP];
static atomic_uint uac_q_head = 0;
static atomic_uint uac_q_tail = 0;

/* ---------------------------------------------------------------------
 * Module-level state
 * --------------------------------------------------------------------- */
static snd_pcm_t *uac_pcm_handle = NULL; // ALSA PCM write handle - owned
                                         // by uac_writer_thread() only
static int uac_gadget_up = 0;            // 1 after configfs gadget is created
static volatile int uac_active = 0;      // 1 while host is streaming
static pthread_t uac_writer_tid;
static volatile int uac_writer_running = 0; // 1 while uac_writer_thread() should keep looping

/* ---------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------- */

// Write a NUL-terminated string to a sysfs/configfs attribute file.
// Returns 0 on success, -1 on error (errno is preserved).
static int uac_write_attr(const char *path, const char *value) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return -1;
  ssize_t n = write(fd, value, strlen(value));
  close(fd);
  return (n == (ssize_t)strlen(value)) ? 0 : -1;
}

// Create a directory if it does not already exist.
// Mirrors `mkdir -p` for a single level.
static int uac_mkdir(const char *path) {
  if (mkdir(path, 0755) < 0 && errno != EEXIST)
    return -1;
  return 0;
}

// Create a symbolic link, tolerating EEXIST.
static int uac_symlink(const char *target, const char *link) {
  if (symlink(target, link) < 0 && errno != EEXIST)
    return -1;
  return 0;
}

// Probe for an ALSA card whose /proc/asound/cardN/id matches target_id
// exactly. Sets card_idx to the card number and returns 0 on success,
// -1 if not found.
static int uac_find_card_by_id(const char *target_id, int *card_idx) {
  for (int c = 0; c < 32; c++) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/asound/card%d/id", c);
    FILE *f = fopen(path, "r");
    if (!f)
      continue;
    char id[64] = {0};
    if (fgets(id, sizeof(id), f)) {
      // Strip trailing newline
      id[strcspn(id, "\n")] = '\0';
      if (strcmp(id, target_id) == 0) {
        *card_idx = c;
        fclose(f);
        return 0;
      }
    }
    fclose(f);
  }
  return -1;
}

// Detect the first UDC (USB Device Controller) available on this system by
// listing /sys/class/udc/. Copies the UDC name into buf (max len).
// Returns 0 on success, -1 if no UDC is found.
static int uac_find_udc(char *buf, size_t len) {
  DIR *d = opendir("/sys/class/udc");
  if (!d)
    return -1;
  struct dirent *de;
  while ((de = readdir(d))) {
    if (de->d_name[0] == '.')
      continue;
    snprintf(buf, len, "%s", de->d_name);
    closedir(d);
    return 0;
  }
  closedir(d);
  return -1;
}

/* ---------------------------------------------------------------------
 * Gadget configuration via configfs
 * --------------------------------------------------------------------- */

// Create and configure the UAC2 gadget under configfs.
// Idempotent: if the gadget already exists (leftover from a previous run
// that didn't shut down cleanly - Ctrl+C, systemd restarting us, a crash,
// or SIGKILL - none of which reach uac_stop()/uac_gadget_destroy()), this
// function detects the existing tree and skips redundant mkdir/write calls.
// Returns 0 on success, -1 on any configfs error.
static int uac_gadget_create(void) {
  char path[256];

  // --- Gadget root ---
  if (uac_mkdir(UAC_GADGET_ROOT) < 0) {
    fprintf(stderr, "uac: cannot create gadget root %s: %s\n", UAC_GADGET_ROOT, strerror(errno));
    return -1;
  }

  // Set as soon as a gadget directory exists, not only after a full
  // success, so a failed bind still gets cleaned up by uac_stop() -
  // see docs/dsp_design_notes/usb_gadget_OS_setup.md §8 for the restart bug this fixed.
  uac_gadget_up = 1;

  // Self-heal: unbind a gadget left BOUND by a previous run before
  // reconfiguring/rebinding, or the kernel refuses with EBUSY on every
  // restart after the first - see docs/dsp_design_notes/usb_gadget_OS_setup.md §8.
  // Idempotent even when nothing was actually bound.
  snprintf(path, sizeof(path), "%s/UDC", UAC_GADGET_ROOT);
  {
    FILE *udc_check = fopen(path, "r");
    if (udc_check) {
      char cur[256] = {0};
      if (fgets(cur, sizeof(cur), udc_check))
        cur[strcspn(cur, "\n")] = '\0';
      fclose(udc_check);
      if (cur[0] != '\0') {
        printf("uac: gadget still bound to '%s' from a previous run - unbinding first\n", cur);
        // NOT "" - see uac_gadget_destroy()'s unbind comment below.
        if (uac_write_attr(path, "\n") < 0)
          fprintf(stderr, "uac: unbind write failed: %s\n", strerror(errno));
      }
    }
    // ENOENT here just means no leftover tree at all (first boot,
    // or a prior clean uac_gadget_destroy()) - nothing to unbind.
  }

  // USB IDs: use the HermesLite vendor/product pair to stay compatible with
  // SDR apps that enumerate by USB ID, while the product string distinguishes us.
  uac_write_attr(UAC_GADGET_ROOT "/idVendor", "0x04B4");  // Cypress / generic
  uac_write_attr(UAC_GADGET_ROOT "/idProduct", "0x0008"); // generic audio
  uac_write_attr(UAC_GADGET_ROOT "/bcdUSB", "0x0200");    // USB 2.0
  uac_write_attr(UAC_GADGET_ROOT "/bcdDevice", "0x0100");

  // --- String descriptors (English) ---
  snprintf(path, sizeof(path), "%s/strings/0x409", UAC_GADGET_ROOT);
  uac_mkdir(path);
  snprintf(path, sizeof(path), "%s/strings/0x409/manufacturer", UAC_GADGET_ROOT);
  uac_write_attr(path, "sBitx");
  snprintf(path, sizeof(path), "%s/strings/0x409/product", UAC_GADGET_ROOT);
  uac_write_attr(path, "sBitx IQ");
  snprintf(path, sizeof(path), "%s/strings/0x409/serialnumber", UAC_GADGET_ROOT);
  uac_write_attr(path, "0000001");

  // --- UAC2 function ---
  snprintf(path, sizeof(path), "%s/functions/uac2.0", UAC_GADGET_ROOT);
  if (uac_mkdir(path) < 0 && errno != EEXIST) {
    fprintf(stderr, "uac: cannot create uac2 function: %s\n", strerror(errno));
    return -1;
  }

  // Capture (host reads IQ from us): 2 ch, 24-bit, 48 kHz
  snprintf(path, sizeof(path), "%s/functions/uac2.0/c_srate", UAC_GADGET_ROOT);
  uac_write_attr(path, "48000");
  snprintf(path, sizeof(path), "%s/functions/uac2.0/c_ssize", UAC_GADGET_ROOT);
  uac_write_attr(path, "3"); // 3 bytes = 24-bit PCM
  snprintf(path, sizeof(path), "%s/functions/uac2.0/c_chmask", UAC_GADGET_ROOT);
  uac_write_attr(path, "3"); // bitmask: ch0 | ch1  = L+R

  // Playback (host -> device, unused but the UAC2 function requires it)
  snprintf(path, sizeof(path), "%s/functions/uac2.0/p_srate", UAC_GADGET_ROOT);
  uac_write_attr(path, "48000");
  snprintf(path, sizeof(path), "%s/functions/uac2.0/p_ssize", UAC_GADGET_ROOT);
  uac_write_attr(path, "3");
  snprintf(path, sizeof(path), "%s/functions/uac2.0/p_chmask", UAC_GADGET_ROOT);
  uac_write_attr(path, "3");

  // --- ACM (CDC-ACM) function: Kenwood TS-480-subset CAT control ---
  // See the CAT section near the bottom of this file and
  // docs/04_remote_control_and_iq_output.md. Unlike
  // uac2.0 above, the kernel's f_acm function has essentially no
  // configurable attributes to set here (just a read-only port_num) -
  // creating the directory is the whole job. Binding this gives us
  // /dev/ttyGS0 on this side; Windows 10/11 auto-binds its inbox
  // usbser.sys driver on the host side with no custom INF, since this
  // presents as a standard bInterfaceClass=0x02/bInterfaceSubClass=0x02
  // CDC-ACM interface - same "just works" story UAC2 already gets for
  // audio class devices.
  snprintf(path, sizeof(path), "%s/functions/acm.usb0", UAC_GADGET_ROOT);
  if (uac_mkdir(path) < 0 && errno != EEXIST) {
    fprintf(stderr, "uac: cannot create acm function: %s\n", strerror(errno));
    return -1;
  }

  // --- Config c.1 ---
  snprintf(path, sizeof(path), "%s/configs/c.1", UAC_GADGET_ROOT);
  uac_mkdir(path);
  snprintf(path, sizeof(path), "%s/configs/c.1/strings/0x409", UAC_GADGET_ROOT);
  uac_mkdir(path);
  snprintf(path, sizeof(path), "%s/configs/c.1/strings/0x409/configuration", UAC_GADGET_ROOT);
  uac_write_attr(path, "Default");
  snprintf(path, sizeof(path), "%s/configs/c.1/bmAttributes", UAC_GADGET_ROOT);
  uac_write_attr(path, "0xC0"); // self-powered + bus-powered
  snprintf(path, sizeof(path), "%s/configs/c.1/MaxPower", UAC_GADGET_ROOT);
  uac_write_attr(path, "250"); // 250 x 2 mA = 500 mA

  // --- Link function into config ---
  char func_abs[256], link_path[256];
  snprintf(func_abs, sizeof(func_abs), "%s/functions/uac2.0", UAC_GADGET_ROOT);
  snprintf(link_path, sizeof(link_path), "%s/configs/c.1/uac2.0", UAC_GADGET_ROOT);
  uac_symlink(func_abs, link_path);

  snprintf(func_abs, sizeof(func_abs), "%s/functions/acm.usb0", UAC_GADGET_ROOT);
  snprintf(link_path, sizeof(link_path), "%s/configs/c.1/acm.usb0", UAC_GADGET_ROOT);
  uac_symlink(func_abs, link_path);

  // --- Bind to the UDC ---
  char udc_name[256] = {0}; // sized to match dirent.d_name's worst case
  if (uac_find_udc(udc_name, sizeof(udc_name)) < 0) {
    fprintf(stderr, "uac: no UDC found — USB gadget not available\n");
    // Not a hard failure: minibitx continues over HPSDR/UDP without UAC
    return -1;
  }
  snprintf(path, sizeof(path), "%s/UDC", UAC_GADGET_ROOT);
  if (uac_write_attr(path, udc_name) < 0) {
    fprintf(stderr, "uac: cannot bind to UDC '%s': %s\n", udc_name, strerror(errno));
    return -1;
  }

  printf("init: USB gadget bound to UDC '%s' (UAC2 audio + ACM CAT)\n", udc_name);
  return 0;
}

// Tear down the gadget: unbind from UDC, unlink function, remove configfs nodes.
// A best-effort cleanup — errors are logged but not fatal.
static void uac_gadget_destroy(void) {
  char path[256];

  // Unbind: write a single newline to UDC, NOT a true empty (0-byte)
  // write - a 0-length write doesn't reliably reach the kernel's UDC
  // store callback, matching the shell idiom `echo "" > UDC` (which
  // itself writes one byte) rather than the literal empty string. See
  // docs/dsp_design_notes/usb_gadget_OS_setup.md §8 for the bench story behind this.
  snprintf(path, sizeof(path), "%s/UDC", UAC_GADGET_ROOT);
  uac_write_attr(path, "\n");

  // Remove the function symlinks from the config
  snprintf(path, sizeof(path), "%s/configs/c.1/uac2.0", UAC_GADGET_ROOT);
  unlink(path);
  snprintf(path, sizeof(path), "%s/configs/c.1/acm.usb0", UAC_GADGET_ROOT);
  unlink(path);

  // Remove config strings, config, and the uac2.0 function's own
  // directory - but NOT functions/acm.usb0's own directory. That one
  // is deliberately skipped: bench-confirmed to hang this process
  // forever, unkillable even with SIGKILL (a kernel-side issue in
  // u_serial.c/usb_f_acm.c freeing the gserial/ttyGS0 port on rmdir -
  // see docs/dsp_design_notes/usb_gadget_OS_setup.md §14 for the full kernel stack and
  // writeup). Safe to skip: configfs is in-memory and doesn't survive
  // a reboot, and uac_gadget_create()'s self-heal already tolerates a
  // leftover tree, so restarting on the same boot just reuses the
  // never-freed ACM function instead of recreating it.
  char dirs[4][256];
  snprintf(dirs[0], 256, "%s/configs/c.1/strings/0x409", UAC_GADGET_ROOT);
  snprintf(dirs[1], 256, "%s/configs/c.1", UAC_GADGET_ROOT);
  snprintf(dirs[2], 256, "%s/functions/uac2.0", UAC_GADGET_ROOT);
  snprintf(dirs[3], 256, "%s/strings/0x409", UAC_GADGET_ROOT);
  for (int i = 0; i < 4; i++)
    rmdir(dirs[i]); // silently tolerate ENOTEMPTY / ENOENT

  // Also deliberately skipped: rmdir(UAC_GADGET_ROOT) itself, which
  // would fail anyway (ENOTEMPTY) since acm.usb0 is still there -
  // stated outright here rather than left as a silent ENOTEMPTY.

  printf("uac: gadget partially removed (UDC unbound, config detached; "
         "the ACM/CAT function's own directory is intentionally left in "
         "place until reboot - see the comment above)\n");
}

/* ---------------------------------------------------------------------
 * UAC2 gadget ALSA PCM setup
 * --------------------------------------------------------------------- */

// Open and configure the UAC2 gadget's own ALSA PCM for write (playback
// side). Once bound to a UDC, the kernel's UAC2 function driver
// registers its OWN independent ALSA card, always id "UAC2Gadget" in
// /proc/asound/cards - its device-0 playback PCM is what's actually
// wired to the real USB isochronous endpoint the host reads. An earlier
// version wrote to a separate snd-aloop "Loopback" card instead, on the
// mistaken assumption UAC2 reads its capture side from that
// automatically - it doesn't, so nothing ever reached the USB link; see
// docs/dsp_design_notes/usb_gadget_OS_setup.md §11 for that bench story.
// Returns 0 on success, -1 on ALSA error.
static int uac_alsa_open(void) {
  int card_idx = -1;
  if (uac_find_card_by_id("UAC2Gadget", &card_idx) < 0) {
    fprintf(stderr, "uac: UAC2Gadget ALSA card not found - is the gadget bound to a UDC?\n");
    return -1;
  }

  // Device 0's playback substream - the local write side that feeds the
  // host's capture stream. Device 0's *capture* substream is the reverse
  // direction (host-to-device audio, unused here - see p_srate/p_ssize/
  // p_chmask in uac_gadget_create()), not this device string.
  char dev_name[64];
  snprintf(dev_name, sizeof(dev_name), "hw:%d,0", card_idx);

  int err;
  if ((err = snd_pcm_open(&uac_pcm_handle, dev_name, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
    fprintf(stderr, "uac: snd_pcm_open(%s) failed: %s\n", dev_name, snd_strerror(err));
    uac_pcm_handle = NULL;
    return -1;
  }

  snd_pcm_hw_params_t *hw;
  snd_pcm_hw_params_alloca(&hw);
  snd_pcm_hw_params_any(uac_pcm_handle, hw);

  // Interleaved, 24-bit packed LE, 48 kHz, 2 channels
  snd_pcm_hw_params_set_access(uac_pcm_handle, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  snd_pcm_hw_params_set_format(uac_pcm_handle, hw, SND_PCM_FORMAT_S24_3LE);
  unsigned int rate = UAC_RATE;
  snd_pcm_hw_params_set_rate_near(uac_pcm_handle, hw, &rate, NULL);
  snd_pcm_hw_params_set_channels(uac_pcm_handle, hw, UAC_CHANNELS);

  snd_pcm_uframes_t period = UAC_PERIOD_FRAMES;
  snd_pcm_hw_params_set_period_size_near(uac_pcm_handle, hw, &period, NULL);
  unsigned int periods = UAC_PERIODS;
  snd_pcm_hw_params_set_periods_near(uac_pcm_handle, hw, &periods, NULL);

  if ((err = snd_pcm_hw_params(uac_pcm_handle, hw)) < 0) {
    fprintf(stderr, "uac: snd_pcm_hw_params failed: %s\n", snd_strerror(err));
    snd_pcm_close(uac_pcm_handle);
    uac_pcm_handle = NULL;
    return -1;
  }

  if ((err = snd_pcm_prepare(uac_pcm_handle)) < 0) {
    fprintf(stderr, "uac: snd_pcm_prepare failed: %s\n", snd_strerror(err));
    snd_pcm_close(uac_pcm_handle);
    uac_pcm_handle = NULL;
    return -1;
  }

  printf("uac: UAC2Gadget ALSA PCM opened: %s @ %u Hz, 24-bit, %d ch\n", dev_name, rate,
         UAC_CHANNELS);
  return 0;
}

/* ---------------------------------------------------------------------
 * Writer thread — owns uac_pcm_handle exclusively; the only thing that
 * ever calls snd_pcm_writei() on it. uac_push_iq() (called from
 * sound.c's real-time audio thread) only ever touches the lock-free
 * queue above and never blocks; this thread is the sole consumer,
 * waiting for a full period's worth of samples and writing them to the
 * gadget's PCM, so a blocked or absent USB host can only cost USB audio
 * quality, never the radio's own real hardware timing. Ported from an
 * earlier design that wrote inline from the audio thread and
 * reintroduced sound.c's xrun flood whenever no host was draining the
 * gadget - see docs/dsp_design_notes/usb_gadget_OS_setup.md §7 for that bench story.
 * --------------------------------------------------------------------- */
static void *uac_writer_thread(void *arg) {
  (void)arg;

  // err_streak drives the backoff below; host_was_draining logs state
  // *transitions* only, not every attempt. Both start pessimistic and
  // require sustained evidence before flipping - see
  // docs/dsp_design_notes/usb_gadget_OS_setup.md §11 for why (a naive version logged a
  // spurious transition on every cold boot with no cable connected).
  unsigned err_streak = 0;
  unsigned success_streak = 0;
  unsigned pending_err_streak = 0; // err_streak snapshotted at the start
                                   // of the current run of successes,
                                   // for the "draining again after N
                                   // failed writes" message once that
                                   // run is confirmed real (below)
  int host_was_draining = 0;

  while (uac_writer_running) {
    unsigned head = atomic_load_explicit(&uac_q_head, memory_order_acquire);
    unsigned tail = atomic_load_explicit(&uac_q_tail, memory_order_relaxed);
    unsigned available = (head - tail) & UAC_QUEUE_MASK;

    if (available < UAC_BUF_FRAMES) {
      // Not enough queued yet - this thread isn't real-time
      // critical (see above), so a plain short sleep is fine.
      struct timespec ts = {.tv_sec = 0, .tv_nsec = 2000000L}; // 2ms
      nanosleep(&ts, NULL);
      continue;
    }

    // Back off the retry pace itself, not just the logging, while no
    // host is draining the gadget - ramps from one period up to a
    // ~1s cap as the failure streak grows, reset immediately on a
    // success. See docs/dsp_design_notes/usb_gadget_OS_setup.md §11 for why (retrying
    // at full pace forever would make "no cable" a low-grade cost
    // instead of the fully-supported idle state it's meant to be).
    if (err_streak > 0) {
      unsigned backoff_periods = err_streak;
      if (backoff_periods > 100)
        backoff_periods = 100; // cap ~1.07s
      struct timespec ts = {.tv_sec = 0, .tv_nsec = 10700000L};
      for (unsigned b = 0; b < backoff_periods && uac_writer_running; b++)
        nanosleep(&ts, NULL);
      if (!uac_writer_running)
        break;
    }

    for (int s = 0; s < UAC_BUF_FRAMES; s++) {
      double i_val = uac_q_i[tail];
      double q_val = uac_q_q[tail];
      tail = (tail + 1) & UAC_QUEUE_MASK;

      // Clamp to [-1, 1] before conversion
      if (i_val > 1.0)
        i_val = 1.0;
      if (i_val < -1.0)
        i_val = -1.0;
      if (q_val > 1.0)
        q_val = 1.0;
      if (q_val < -1.0)
        q_val = -1.0;

      // Scale to 24-bit signed integer range and pack as 3-byte
      // little-endian (SND_PCM_FORMAT_S24_3LE: [LSB, mid, MSB])
      int32_t i_int = (int32_t)(i_val * 8388607.0); // 2^23 - 1
      int32_t q_int = (int32_t)(q_val * 8388607.0);

      uint8_t *slot = uac_pcm_buf + s * UAC_FRAME_BYTES;
      // I sample (left channel)
      slot[0] = (uint8_t)(i_int & 0xFF);
      slot[1] = (uint8_t)((i_int >> 8) & 0xFF);
      slot[2] = (uint8_t)((i_int >> 16) & 0xFF);
      // Q sample (right channel)
      slot[3] = (uint8_t)(q_int & 0xFF);
      slot[4] = (uint8_t)((q_int >> 8) & 0xFF);
      slot[5] = (uint8_t)((q_int >> 16) & 0xFF);
    }
    atomic_store_explicit(&uac_q_tail, tail, memory_order_release);

    // Flush a full period to the gadget's PCM. This can block (or
    // fail) if nothing is draining the other side - that's now
    // confined to this thread only.
    snd_pcm_sframes_t written =
        snd_pcm_writei(uac_pcm_handle, uac_pcm_buf, (snd_pcm_uframes_t)UAC_BUF_FRAMES);

    if (written == -EPIPE) {
      // Buffer underrun (most commonly: no host draining the
      // gadget yet) - attempt recovery then retry once.
      snd_pcm_prepare(uac_pcm_handle);
      written = snd_pcm_writei(uac_pcm_handle, uac_pcm_buf, (snd_pcm_uframes_t)UAC_BUF_FRAMES);
    }

    if (written < 0) {
      // Most commonly means no USB host has activated the capture
      // interface yet (cable unplugged, or no app has opened it) -
      // a normal, expected state, not a fault, so log the
      // *transition* into it once rather than every retry. See
      // docs/dsp_design_notes/usb_gadget_OS_setup.md §11.
      if (host_was_draining) {
        fprintf(stderr,
                "uac: no USB host draining the gadget yet (%s) - "
                "will keep retrying quietly in the background\n",
                snd_strerror((int)written));
        host_was_draining = 0;
      }
      success_streak = 0;
      err_streak++;
      snd_pcm_recover(uac_pcm_handle, (int)written, 1 /*silent*/);
    } else {
      if (success_streak == 0) {
        // First success of a new run - remember how many writes
        // failed right before it, for the "draining again after
        // N failed writes" message below, once/if this run turns
        // out to be real (not just the buffer swallowing a few
        // writes for free with nothing on the other end - see
        // the success_streak threshold below). err_streak itself
        // gets reset right away regardless, since the backoff
        // pacing above needs to see "no current failure streak"
        // as soon as a write succeeds.
        pending_err_streak = err_streak;
      }
      success_streak++;
      err_streak = 0;

      // Require more consecutive successes than UAC_PERIODS before
      // trusting it as proof of a draining host - see
      // docs/dsp_design_notes/usb_gadget_OS_setup.md §11 for why a short run can
      // succeed purely from empty-buffer slack.
      if (!host_was_draining && success_streak > UAC_PERIODS) {
        if (pending_err_streak > 0) {
          fprintf(stderr, "uac: USB host draining again after %u failed write(s)\n",
                  pending_err_streak);
        } else {
          fprintf(stderr, "uac: USB host draining the gadget\n");
        }
        host_was_draining = 1;
      }
    }
  }

  return NULL;
}

/* ---------------------------------------------------------------------
 * Public API — see usb_gadget.h
 * --------------------------------------------------------------------- */

int uac_init(void) {
  if (uac_gadget_create() < 0) {
    // uac_gadget_create already printed the reason. Note: it may
    // still have left a gadget directory that needs cleanup - it
    // sets uac_gadget_up itself now (as soon as it creates one),
    // precisely so a failed bind here doesn't skip that cleanup.
    return -1;
  }

  if (uac_alsa_open() < 0) {
    uac_gadget_destroy();
    uac_gadget_up = 0;
    return -1;
  }

  uac_writer_running = 1;
  if (pthread_create(&uac_writer_tid, NULL, uac_writer_thread, NULL) != 0) {
    fprintf(stderr, "uac: failed to start writer thread\n");
    uac_writer_running = 0;
    snd_pcm_close(uac_pcm_handle);
    uac_pcm_handle = NULL;
    uac_gadget_destroy();
    uac_gadget_up = 0;
    return -1;
  }

  uac_active = 1;
  printf("uac: USB IQ audio stream ready — device name: 'sBitx IQ'\n");
  return 0;
}

void uac_push_iq(double i_val, double q_val) {
  if (!uac_active)
    return;

  // Lock-free producer (see the writer-thread comment above): never
  // blocks, never touches uac_q_tail. On overflow (writer thread
  // stalled behind a slow/absent USB host) it silently drops the new
  // sample rather than waiting - exactly hpsdr_p1.c's
  // hpsdr_send_iq()/IQ_QUEUE pattern.
  unsigned head = atomic_load_explicit(&uac_q_head, memory_order_relaxed);
  unsigned tail = atomic_load_explicit(&uac_q_tail, memory_order_acquire);
  unsigned next_head = (head + 1) & UAC_QUEUE_MASK;
  if (next_head == tail)
    return; // queue full - drop this sample

  uac_q_i[head] = i_val;
  uac_q_q[head] = q_val;
  atomic_store_explicit(&uac_q_head, next_head, memory_order_release);
}

void uac_stop(void) {
  uac_active = 0;

  if (uac_writer_running) {
    uac_writer_running = 0;
    pthread_join(uac_writer_tid, NULL);
  }

  if (uac_pcm_handle) {
    snd_pcm_drain(uac_pcm_handle);
    snd_pcm_close(uac_pcm_handle);
    uac_pcm_handle = NULL;
    printf("uac: ALSA PCM closed\n");
  }

  if (uac_gadget_up) {
    uac_gadget_destroy();
    uac_gadget_up = 0;
  }
}

int uac_is_active(void) { return uac_active; }

/* =======================================================================
 * Kenwood TS-480-subset CAT control, over this same gadget's CDC-ACM
 * function acm.usb0 (created/bound in uac_gadget_create() above; see
 * usb_gadget.h for why this is folded into the same file rather than a
 * separate translation unit). See docs/04_remote_control_and_iq_output.md
 * for the command set and why TS-480, and docs/dsp_design_notes/usb_gadget_OS_setup.md
 * §13 for the bench-test checklist. Best-effort like the rest of this
 * file: cat_init() failing is not fatal.
 *
 * Unlike hamlib.c's TCP server, no accept()/one-thread-per-client model
 * is needed - a USB gadget serial function is inherently one logical
 * connection at a time, so this is a single persistent reader thread
 * that opens the device once and re-opens it if it ever drops.
 * ======================================================================= */

#define CAT_TTY_PATH "/dev/ttyGS0"
#define CAT_LINE_MAX 64
#define CAT_OPEN_RETRY_MAX_MS 1000 // backoff cap while the device is missing/erroring

static volatile int cat_running = 0;
static int cat_fd = -1;
static pthread_t cat_thread_tid;

// Cosmetic-only "current mode" state, same idea as hamlib.c's current_mode -
// minibitx has no onboard demod and (as of this writing) can only actually
// transmit CW, so "3" (Kenwood's MD code for CW) is the honest default
// rather than pretending to support modes nothing downstream can produce.
// MD set requests are still accepted and stored, same as Hamlib's M/m, in
// case a future TX audio path (see the FLRig/WSJT-X design discussion)
// makes other modes real.
static char cat_current_mode[2] = "3";

static void cat_send(const char *s) {
  if (cat_fd < 0)
    return;
  // Best-effort - a port that's not actually open on the host side isn't
  // fatal, same spirit as hamlib.c's send_line() using MSG_NOSIGNAL.
  ssize_t n = write(cat_fd, s, strlen(s));
  (void)n;
}

// Handles one already-terminated command (the ';' stripped). Kenwood CAT
// convention, unlike Hamlib's rigctld: "set" commands get no reply,
// only "get" queries do - so most branches below send nothing.
//
// A get's console line only prints when the reply actually differs from
// the last reply of the SAME kind, to avoid flooding the console with
// FLRig's frequent status polling - see docs/04_remote_control_and_iq_output.md
// "Kenwood-CAT emulation over USB" for why. Pass a `static char[]` local
// to each call site (persists across calls, scoped to that command) as
// `last`. Only used on the get side - a *set* always logs.
static void cat_log_get(char *last, size_t last_size, const char *new_reply,
                        const char *log_line) {
  if (strncmp(last, new_reply, last_size) == 0)
    return;
  snprintf(last, last_size, "%s", new_reply); // truncates+NUL-terminates safely
  printf("%s", log_line);
}

static void cat_handle_command(char *cmd) {
  size_t len = strlen(cmd);
  if (len == 0)
    return;

  // --- ID: get only - identify as a Kenwood TS-480 (ID code 020),
  // matching the QRP Labs QMX's own choice for the same reason: FLRig
  // (or anything else) will expect TS-480 behavior from every other
  // command once it sees this. Never actually changes, so this only
  // ever logs once (see cat_log_get() above). ---
  if (len == 2 && strncmp(cmd, "ID", 2) == 0) {
    static char last[8] = "";
    cat_send("ID020;");
    cat_log_get(last, sizeof(last), "ID020;", "cat: ID -> 020 (TS-480)\n");
    return;
  }

  // --- AC: antenna tuner control, part of FLRig's standard status
  // poll - minibitx has no tuner to report on, so silently ignored
  // rather than logged every poll forever. See docs/04's
  // "Kenwood-CAT emulation over USB". ---
  if (len >= 2 && cmd[0] == 'A' && cmd[1] == 'C') {
    return;
  }

  // --- FA: VFO A frequency, 11-digit Hz. Bare "FA" is a get; "FA<11
  // digits>" is a set. ---
  if (len >= 2 && cmd[0] == 'F' && cmd[1] == 'A') {
    if (len == 2) {
      static char last[16] = "";
      char buf[16], log_line[48];
      snprintf(buf, sizeof(buf), "FA%011d;", freq_hdr);
      cat_send(buf);
      snprintf(log_line, sizeof(log_line), "cat: FA -> %d Hz\n", freq_hdr);
      cat_log_get(last, sizeof(last), buf, log_line);
    } else {
      long f = strtol(cmd + 2, NULL, 10);
      if (f > 0) {
        radio_tune_to((uint32_t)f);
        printf("cat: FA%s -> tuned to %ld Hz\n", cmd + 2, f);
      } else {
        printf("cat: FA%s -> invalid frequency, ignored\n", cmd + 2);
      }
    }
    return;
  }

  // --- FB: VFO B. minibitx has one VFO - mirror FA on get, accept and
  // ignore on set, same "nothing else to switch to" stance Hamlib's V/
  // chk_vfo already takes. ---
  if (len >= 2 && cmd[0] == 'F' && cmd[1] == 'B') {
    if (len == 2) {
      static char last[16] = "";
      char buf[16], log_line[64];
      snprintf(buf, sizeof(buf), "FB%011d;", freq_hdr);
      cat_send(buf);
      snprintf(log_line, sizeof(log_line), "cat: FB -> %d Hz (single VFO, mirrors FA)\n",
               freq_hdr);
      cat_log_get(last, sizeof(last), buf, log_line);
    } else {
      printf("cat: FB%s -> ok (single VFO, not applied)\n", cmd + 2);
    }
    return;
  }

  // --- TX / RX: bare, immediate, no reply (Kenwood convention) - same
  // entry point and same "local CW key wins" guard as Hamlib's T. ---
  if (len == 2 && strncmp(cmd, "TX", 2) == 0) {
    if (cw_tx_active()) {
      printf("cat: TX -> ignored, local CW key holds TX\n");
    } else {
      radio_set_tx(1);
      printf("cat: TX -> TX on\n");
    }
    return;
  }
  if (len == 2 && strncmp(cmd, "RX", 2) == 0) {
    if (cw_tx_active()) {
      printf("cat: RX -> ignored, local CW key holds TX\n");
    } else {
      radio_set_tx(0);
      printf("cat: RX -> TX off\n");
    }
    return;
  }

  // --- TQ: transmit state - "TQ;" is a get (replies), "TQ0;"/"TQ1;" is
  // a set (no reply, same as TX/RX above - this is just another way to
  // ask for the same thing). ---
  if (len >= 2 && cmd[0] == 'T' && cmd[1] == 'Q') {
    if (len == 2) {
      static char last[8] = "";
      char buf[8], log_line[32];
      snprintf(buf, sizeof(buf), "TQ%d;", in_tx ? 1 : 0);
      cat_send(buf);
      snprintf(log_line, sizeof(log_line), "cat: TQ -> %s\n", in_tx ? "TX" : "RX");
      cat_log_get(last, sizeof(last), buf, log_line);
    } else {
      int tx_on = (cmd[2] != '0');
      if (cw_tx_active()) {
        printf("cat: TQ%c -> ignored, local CW key holds TX\n", cmd[2]);
      } else {
        radio_set_tx(tx_on);
        printf("cat: TQ%c -> %s\n", cmd[2], tx_on ? "TX on" : "TX off");
      }
    }
    return;
  }

  // --- MD: mode - cosmetic only, see cat_current_mode's comment above. ---
  if (len >= 2 && cmd[0] == 'M' && cmd[1] == 'D') {
    if (len == 2) {
      static char last[8] = "";
      char buf[8], log_line[32];
      snprintf(buf, sizeof(buf), "MD%s;", cat_current_mode);
      cat_send(buf);
      snprintf(log_line, sizeof(log_line), "cat: MD -> %s\n", cat_current_mode);
      cat_log_get(last, sizeof(last), buf, log_line);
    } else {
      cat_current_mode[0] = cmd[2];
      cat_current_mode[1] = '\0';
      printf("cat: MD%c -> ok (cosmetic, not applied)\n", cmd[2]);
    }
    return;
  }

  // --- IF: combined status string - get only. Best-effort reconstruction
  // of the classic Kenwood IF layout (11-digit freq, 5-char step/blank,
  // 5-char signed RIT offset, RIT on/off, XIT on/off, memory bank,
  // 2-digit memory channel, TX/RX, mode, VFO/memory, scan, split, tone
  // status, 2-digit tone number, one reserved digit) with everything
  // minibitx doesn't have (RIT/XIT/memory/scan/split/tone) reported as
  // off/zero. NOTE: the exact field widths here are reconstructed from
  // the general Kenwood IF convention, not confirmed character-for-
  // character against QMX's own manual text (only a paraphrased summary
  // of it was available while writing this) - if FLRig's status display
  // looks wrong (frequency in the wrong place, mode misread) while FA/
  // MD/TQ individually work fine, this is the first place to check,
  // ideally against a packet capture of a real QMX's IF response.
  if (len == 2 && strncmp(cmd, "IF", 2) == 0) {
    static char last[40] = "";
    char buf[40], log_line[80];
    snprintf(buf, sizeof(buf), "IF%011d00000+0000000%02d%d%s00000000;", freq_hdr,
             0 /* memory channel */, in_tx ? 1 : 0, cat_current_mode);
    cat_send(buf);
    snprintf(log_line, sizeof(log_line), "cat: IF -> sent (freq %d, %s, mode %s)\n", freq_hdr,
             in_tx ? "TX" : "RX", cat_current_mode);
    cat_log_get(last, sizeof(last), buf, log_line);
    return;
  }

  // Unknown command - Kenwood radios generally stay silent on anything
  // they don't recognize (no Hamlib-style "unknown command" reply
  // convention exists here), so match that rather than invent one.
  // Logging is still throttled to repeat-suppression (not full
  // silence, unlike AC above) since an unrecognized command here is
  // more likely a genuine gap worth noticing than AC's known-benign
  // poll - but a client stuck retrying the very same unrecognized
  // command every cycle (as FLRig already does for AC, and might for
  // some other command not yet seen) shouldn't get a fresh line every
  // single time either.
  {
    static char last_unrecognized[CAT_LINE_MAX] = "";
    if (strncmp(last_unrecognized, cmd, sizeof(last_unrecognized)) != 0) {
      snprintf(last_unrecognized, sizeof(last_unrecognized), "%s", cmd);
      printf("cat: %s -> unrecognized, ignored\n", cmd);
    }
  }
}

// Puts the ACM tty into raw mode: no line discipline, no echo, one byte
// read at a time (VMIN=1/VTIME=0). Without this, the kernel's tty layer
// applies ordinary canonical-mode line editing/echo to what's actually a
// binary-ish, semicolon-terminated protocol with no real newlines -
// harmless-looking in a first read, but silently wrong, the same class of
// "looks fine, isn't" bug as this file's 0-byte-vs-1-byte UDC unbind write
// earlier in this project.
static void cat_set_raw(int fd) {
  struct termios tio;
  if (tcgetattr(fd, &tio) < 0)
    return;
  cfmakeraw(&tio);
  tio.c_cc[VMIN] = 1;
  tio.c_cc[VTIME] = 0;
  tcsetattr(fd, TCSANOW, &tio);
}

static void *cat_thread_fn(void *arg) {
  (void)arg;
  char buf[CAT_LINE_MAX];
  size_t buf_len = 0;
  unsigned open_backoff_ms = 10;

  while (cat_running) {
    if (cat_fd < 0) {
      cat_fd = open(CAT_TTY_PATH, O_RDWR | O_NOCTTY);
      if (cat_fd < 0) {
        // Most commonly: the ACM gadget function isn't bound yet
        // (or at all). Back off rather than spin - same lesson as
        // this file's UAC2 writer-thread fix: an absent/not-yet-
        // ready consumer/producer must not cost CPU forever.
        struct timespec ts = {.tv_sec = open_backoff_ms / 1000,
                              .tv_nsec = (open_backoff_ms % 1000) * 1000000L};
        nanosleep(&ts, NULL);
        if (open_backoff_ms < CAT_OPEN_RETRY_MAX_MS)
          open_backoff_ms *= 2;
        continue;
      }
      cat_set_raw(cat_fd);
      open_backoff_ms = 10; // reset now that it's open again
      buf_len = 0;
      printf("cat: %s opened\n", CAT_TTY_PATH);
    }

    char c;
    ssize_t n = read(cat_fd, &c, 1);
    if (n <= 0) {
      // Device gone (gadget unbound/rebound, cable pulled) - close
      // and let the top of the loop reopen it with backoff.
      if (cat_running)
        printf("cat: %s closed, will retry\n", CAT_TTY_PATH);
      close(cat_fd);
      cat_fd = -1;
      continue;
    }

    if (c == ';') {
      buf[buf_len] = '\0';
      cat_handle_command(buf);
      buf_len = 0;
    } else if (c != '\r' && c != '\n' && buf_len + 1 < sizeof(buf)) {
      buf[buf_len++] = c;
    }
    // else: stray CR/LF between commands, or a line too long - drop it
    // silently until the next ';'.
  }

  if (cat_fd >= 0) {
    close(cat_fd);
    cat_fd = -1;
  }
  return NULL;
}

int cat_init(void) {
  cat_running = 1;
  if (pthread_create(&cat_thread_tid, NULL, cat_thread_fn, NULL) != 0) {
    fprintf(stderr, "cat: failed to start reader thread\n");
    cat_running = 0;
    return -1;
  }
  // Detached, not joined - see cat_stop()'s comment; joining this
  // thread was tried and reintroduced §14's unkillable shutdown hang.
  pthread_detach(cat_thread_tid);
  printf("init: CAT (Kenwood TS-480 subset) listening on %s\n", CAT_TTY_PATH);
  return 0;
}

void cat_stop(void) {
  cat_running = 0;
  if (cat_fd >= 0) {
    // Unblock the reader thread's blocking read() - same idea as
    // hamlib_stop() closing listen_fd to unblock accept().
    close(cat_fd);
    cat_fd = -1;
  }

  // Deliberately does NOT pthread_join() the reader thread - an
  // earlier version did, and bench-confirmed it turns into the same
  // unkillable shutdown hang as §14 of docs/dsp_design_notes/usb_gadget_OS_setup.md,
  // just relocated into cat_stop(). Leaving it detached means that
  // hang, if it recurs, hangs alone rather than taking shutdown down
  // with it; §14's rmdir() workaround doesn't depend on this thread
  // having fully exited first, so nothing downstream needs that
  // guarantee.
}
