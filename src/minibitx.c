// minibitx.c
//
// A small application that initializes the sbitx radio hardware, and allows 
// remote SDR applications to control its operation over network or USB 
// connections.

#include "hpsdr_p1.h"
#include "si5351.h"
#include "sound.h"
#include "vfo.h"
#include "radio.h"
#include "radio_hw.h"
#include "usb_gadget.h"
#include "hamlib.h"
#include "hw_settings.h"
#include "cw.h"
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
 
// Standard rigctld TCP port
#define HAMLIB_PORT 4532
 
// Graceful shutdown on SIGINT/SIGTERM - see
// docs/05_process_and_threading_model.md for why this exists (the
// default action used to skip every _stop() below entirely) and
// docs/dsp_design_notes/usb_gadget_OS_setup.md §8 for the independent
// self-healing backstop that still covers SIGKILL/a crash, which can't
// be caught here. The handler only sets a flag - must stay
// async-signal-safe, no printf/pthread/ALSA calls here - the idle loop
// below does the actual teardown once it notices.
static volatile sig_atomic_t shutdown_requested = 0;
 
static void handle_shutdown_signal(int sig) {
  (void)sig;
  shutdown_requested = 1;
}
 
// Every hardware/subsystem init step below reports its own result with a
// consistent "init: ..." line (see docs/01_hardware_init_and_control.md),
// ending in the "radio hardware initialization complete" line. After initialization.
// operational state is reported as it's processed
int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
 
  printf("Starting miniBitx IQ Streamer and control interface...\n");
 
  // Installed first, before anything below can fail/return early - every
  // _stop() function called from the shutdown sequence already guards on
  // its own "did this subsystem actually come up" state (e.g. sound.c's
  // g_running, hpsdr_p1.c's hpsdr_sock >= 0), so it's safe to reach the
  // idle loop and shut down cleanly even if some earlier init step above
  // was skipped or failed.
  struct sigaction sa = {0};
  sa.sa_handler = handle_shutdown_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
 
  // Board-specific calibration (xtal_filter_center and bfo_freq) lives
  // in data/hw_settings.ini, not in source - the crystal filter center
  // varies radio to radio. Load it before anything below uses either.
  hw_settings_load();
 
  // Claim all GPIO lines (LPF relays, TX_LINE, TX_POWER, EXT_PTT, CW_KEY)
  // via the kernel's GPIO character-device API (src/gpio.c) and put the
  // outputs into their idle state.
  if (radio_hw_gpio_init() < 0) {
    fprintf(stderr, "init: GPIO setup failed\n");
    return -1;
  }
  printf("init: GPIO configured, T/R relay and PTT held low (RX-safe state)\n");
 
  // Initialize the si5351 clock generator (this also brings up the I2C
  // bus it needs). si5351bx_init() powers down all three clocks, so
  // clk1 has to be started here before anything downstream needs it -
  // at its RX value; it only switches to bfo_freq during a TX burst
  // (radio_tx_apply(), radio.c) - see
  // docs/01_hardware_init_and_control.md §3 for why RX/TX need
  // different clk1 values.
  si5351bx_init();
  si5351bx_setfreq(1, xtal_filter_center + RX_IF_FREQ_HZ);
  si5351_reset();
  printf("init: si5351 oscillator ready, clk1 (RX) at %d Hz\n",
         xtal_filter_center + RX_IF_FREQ_HZ);
 
  // Board revision and the INA260 power monitor both live on the same I2C
  // bus si5351bx_init() just brought up, so they can only be probed after
  // it, not before.
  int hw_rev = radio_hw_detect_version();
  printf("init: board revision detected: %s\n",
         hw_rev == SBITX_V2 ? "sBitx v2 (power/SWR bridge present)"
                             : "sBitx DE (original, no power/SWR bridge)");
 
  if (radio_hw_ina260_configure() == 0) {
    printf("init: INA260 power monitor configured\n");
  } else {
    printf("init: INA260 power monitor not responding, continuing without it\n");
  }
 
  // Then bring up the software RX VFO's phase table and tune to the
  // startup frequency - radio_tune_to() starts the software oscillator
  // itself, fixed at RX_IF_FREQ_HZ (see radio.h).
  vfo_init_phase_table();
  vfo_start(&lo, RX_IF_FREQ_HZ, 0);
  radio_tune_to(freq_hdr);
  printf("init: RX VFO started, tuned to %d Hz\n", freq_hdr);
 
  // Straight-key CW support (src/cw.c) - needs the phase table above
  // already built, since it starts its software oscillator.
  cw_init();
  printf("init: CW straight key ready (GPIO %d)\n", CW_KEY);
 
  // Bring up the rigctld-compatible control surface (src/hamlib.c) first -
  // not a hard failure if the port's unavailable, same as HPSDR/UAC2.
  // hamlib_init() reports its own success; we only report the failure
  // case here. Started before HPSDR purely so startup mirrors the
  // shutdown order below - see docs/05_process_and_threading_model.md.
  if (hamlib_init(HAMLIB_PORT) < 0) {
    printf("init: Hamlib/rigctld unavailable on TCP %d, continuing without it\n",
           HAMLIB_PORT);
  }
 
  // Initialize Networking (HPSDR Protocol 1)
  if (hpsdr_init() < 0) {
    fprintf(stderr, "init: HPSDR socket bind failed\n");
    return -1;
  }
  hpsdr_poll(); // Starts the listener thread for connection/tuning requests
  printf("init: HPSDR Protocol 1 listening on UDP %d\n", HPSDR_PORT);
 
  // Bring up the USB Audio Class (UAC2) IQ gadget, if the hardware/kernel
  // support it (needs a USB device-mode controller and libcomposite). Not a
  // hard failure if it's unavailable - minibitx keeps running over
  // HPSDR/UDP either way. uac_init() reports its own success ("init: USB
  // IQ gadget bound to UDC...").
  if (uac_init() < 0) {
    printf("init: USB IQ gadget unavailable, continuing without it\n");
  }
 
  // Bring up the Kenwood TS-480-subset CAT control surface (the CAT
  // section of src/usb_gadget.c), over the same gadget's CDC-ACM
  // function uac_init() just created above. Independent from here on:
  // cat_init() opens /dev/ttyGS0 itself on its own thread and keeps
  // retrying with backoff regardless of whether the ACM function
  // actually came up - not a hard failure, same as every control
  // surface here.
  if (cat_init() < 0) {
    printf("init: CAT (ACM) unavailable, continuing without it\n");
  }
 
  // Initialize Audio
  setup_audio_codec();
  printf("init: WM8731 audio codec configured\n");
  // this starts the background audio thread which repeatedly calls sound_process()
  if (sound_thread_start("hw:0,0") < 0) {
    fprintf(stderr, "init: audio capture failed to start\n");
    return -1;
  }
  printf("init: audio capture running (hw:0,0 @ 96000 Hz)\n");
 
  printf("minibitx: radio hardware initialization complete, ready to serve!\n");
 
  // minibitx idle loop: keep the program alive until asked to shut down.
  // Operational state changes (tuning, PTT) are reported as they're
  // processed by hamlib.c/hpsdr_p1.c, not polled here. sleep(1) returns
  // early the moment SIGINT/SIGTERM arrives, so shutdown starts
  // immediately rather than waiting out the rest of that second.
  while (!shutdown_requested) {
    sleep(1);
  }
 
  // Graceful shutdown - roughly the reverse of bring-up, so each step
  // tears down into a quiescent system rather than racing something
  // still running above it.
  printf("\nminibitx: shutting down...\n");
 
  // Park PTT/the T/R relay low in case the key happened to be down at
  // the moment of the signal. radio_set_tx() only hands the change to
  // its own worker thread (see radio.c) rather than applying it
  // synchronously, so give that thread a moment to actually run before
  // this process exits out from under it - 50ms comfortably covers its
  // ~25ms worst-case PTT/relay-settling sequence.
  radio_set_tx(0);
  usleep(50000);
 
  // Stop producing audio/IQ before tearing down anything that consumes
  // it, so uac_stop()/hpsdr_stop() below see a stream that's already
  // quiet rather than racing sound.c's real-time thread mid-teardown.
  sound_thread_stop();
  cat_stop();
  uac_stop();
  hpsdr_stop();
  hamlib_stop();
 
  printf("minibitx: shutdown complete.\n");
  return 0;
}
 
