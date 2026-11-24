// DualSense haptics (F1 > Controls, TOTK_HAPTICS): the game's HD rumble (two bands of amplitude and frequency per side)
// is played as waveforms on the DualSense's voice-coil actuators instead of SDL's motor emulation.
// Bluetooth: output report 0x32 (142 bytes), 32 stereo frames of signed 8-bit samples at 3 kHz, left channel to the left
// actuator, one report per 10.67 ms (layout as DS5Dongle's audio_send_haptics_only, MIT, describes it; our own code).
// USB: the controller's 4-channel audio device at 48 kHz; channels 3 and 4 drive the left and right actuators.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "platform/platform.h"

#define BT_RATE 3000.0f
#define BT_FRAMES 32
#define BT_PERIOD_NS 10666667ull        // BT_FRAMES / BT_RATE
#define REPORT_LEN 142
#define CTRL_BUFFER 48                  // the controller's haptics buffer, frames
#define USB_RATE 48000
#define USB_FRAMES 480                  // 10 ms per push
#define USB_PERIOD_NS 10000000ull
#define USB_QUEUE_MS 30                 // audio kept queued ahead
#define STALE_NS 250000000ull           // a side the game has not updated for 250 ms is silent
#define TWO_PI 6.28318531f

typedef struct { float amp_low, freq_low, amp_high, freq_high; uint64_t at; } Side;

static PlatMutex lock = PLAT_MUTEX_INIT;
static Side want[2];                    // lock
static volatile int32_t ready;          // 1 while the stream owns the actuators (SDL rumble held at zero)
static volatile int32_t started;

int ds5_haptics_ready(void) { return ready != 0; }

void ds5_haptics_set(int side, float al, float fl, float ah, float fh, uint64_t now) {
    if (side < 0 || side > 1) return;
    plat_mutex_lock(&lock);
    want[side] = (Side){al, fl, ah, fh, now};
    plat_mutex_unlock(&lock);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {   // reflected CRC-32 (zlib), bitwise
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return crc;
}

static float clamp01(float v) { return v > 0 ? (v < 1 ? v : 1) : 0; }
static float freq_ok(float f) { return f > 20 ? (f < 1400 ? f : 1400) : 20; }   // below the 3 kHz path's Nyquist

// Per side: the two bands' phases and the amplitudes played last, ramped toward the wanted ones over a block.
typedef struct { float ph_low, ph_high, amp_low, amp_high; } Osc;

// n stereo frames (L, R) in -1..1 at rate; 1 when either side is audible.
// Each band's amplitude goes through curve (the Vibration curve: HD rumble is tuned for actuators that are felt at low
// amplitude, so weak values are lifted as for the motors), then gain.
static int synth(Osc osc[2], const Side cur[2], uint64_t now, float gain, float curve, float rate, int n, float *out) {
    int loud = 0;
    for (int s = 0; s < 2; s++) {
        Osc *o = &osc[s];
        int live = cur[s].at && now - cur[s].at <= STALE_NS;
        float tl = live ? powf(clamp01(cur[s].amp_low), curve) : 0, th = live ? powf(clamp01(cur[s].amp_high), curve) : 0;
        float dl = TWO_PI * freq_ok(cur[s].freq_low) / rate, dh = TWO_PI * freq_ok(cur[s].freq_high) / rate;
        for (int i = 0; i < n; i++) {
            float t = (float)(i + 1) / (float)n;
            float al = o->amp_low + (tl - o->amp_low) * t, ah = o->amp_high + (th - o->amp_high) * t;
            float v = (al * sinf(o->ph_low) + ah * sinf(o->ph_high)) * gain;
            out[i * 2 + s] = v > 1 ? 1 : v < -1 ? -1 : v;
            o->ph_low += dl, o->ph_high += dh;
            if (o->ph_low > TWO_PI) o->ph_low -= TWO_PI;
            if (o->ph_high > TWO_PI) o->ph_high -= TWO_PI;
        }
        o->amp_low = tl, o->amp_high = th;
        if (tl > 0.002f || th > 0.002f) loud = 1;
    }
    return loud;
}

static void build(uint8_t r[REPORT_LEN], unsigned seq, unsigned counter, const float pcm[BT_FRAMES * 2]) {
    memset(r, 0, REPORT_LEN);
    r[0] = 0x32;
    r[1] = (uint8_t)((seq & 15) << 4);
    r[2] = 0x91, r[3] = 4;              // sub-packet 0x11 (sized), 4 bytes:
    r[4] = 0x66;                        //   flags (bit 5: apply the buffer length)
    r[5] = r[6] = CTRL_BUFFER;          //   buffer length
    r[7] = (uint8_t)counter;            //   packet counter
    r[8] = 0x92, r[9] = BT_FRAMES * 2;  // sub-packet 0x12 (sized): the samples
    for (int i = 0; i < BT_FRAMES * 2; i++) r[10 + i] = (uint8_t)(int8_t)lrintf(pcm[i] * 127);
    uint8_t seed = 0xA2;                // Bluetooth HID output header, covered by the CRC
    uint32_t crc = crc32_update(crc32_update(0xFFFFFFFFu, &seed, 1), r, REPORT_LEN - 4) ^ 0xFFFFFFFFu;
    r[138] = (uint8_t)crc, r[139] = (uint8_t)(crc >> 8), r[140] = (uint8_t)(crc >> 16), r[141] = (uint8_t)(crc >> 24);
}

static int is_dualsense(uint16_t vid, uint16_t pid) { return vid == 0x054C && (pid == 0x0CE6 || pid == 0x0DF2); }

// USB: the DualSense's playback device (named "... Wireless Controller" / "DualSense ..."), opened at 4 channels. A
// device Windows set up with fewer channels would get the haptics mixed into its speaker, so it is not used.
static SDL_AudioStream *usb_open(void) {
    static int audio_ok = -1, logged;
    if (audio_ok < 0) audio_ok = SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (!audio_ok) { if (!logged++) rt_log("haptics: no SDL audio (%s)", SDL_GetError()); return NULL; }
    int n = 0;
    SDL_AudioDeviceID *ids = SDL_GetAudioPlaybackDevices(&n), dev = 0;
    for (int i = 0; ids && i < n && !dev; i++) {
        const char *name = SDL_GetAudioDeviceName(ids[i]);
        if (name && (SDL_strcasestr(name, "DualSense") || SDL_strcasestr(name, "Wireless Controller"))) dev = ids[i];
    }
    SDL_free(ids);
    if (!dev) { if (!logged++) rt_log("haptics: the DualSense's audio device was not found; SDL rumble stays"); return NULL; }
    SDL_AudioSpec have;
    int frames = 0;
    if (!SDL_GetAudioDeviceFormat(dev, &have, &frames) || have.channels < 4) {
        if (!logged++) rt_log("haptics: \"%s\" has %d channels, 4 are needed (Windows sound settings: set it to 4 channels);"
                              " SDL rumble stays", SDL_GetAudioDeviceName(dev), have.channels);
        return NULL;
    }
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 4, USB_RATE};
    SDL_AudioStream *st = SDL_OpenAudioDeviceStream(dev, &spec, NULL, NULL);
    if (!st) { if (!logged++) rt_log("haptics: cannot open \"%s\" (%s)", SDL_GetAudioDeviceName(dev), SDL_GetError()); return NULL; }
    SDL_ResumeAudioStreamDevice(st);
    rt_log("haptics: USB audio device \"%s\" (%d channels, %d Hz)", SDL_GetAudioDeviceName(dev), have.channels, have.freq);
    logged = 0;
    return st;
}

static void stream(void *arg) {
    (void)arg;
    SDL_hid_device *hid = NULL;
    SDL_AudioStream *aud = NULL;
    char path[512] = "", open_path[512] = "";
    Osc osc[2] = {{0}};
    unsigned seq = 0, counter = 0, quiet = 0;
    uint64_t next = 0, sent = 0;
    int logged_why = -1, open_failed = 0, first_write = 1, first_loud = 1, quiet_logs = 0;
    static float pcm[USB_FRAMES * 2], quad[USB_FRAMES * 4];
    for (;;) {
        uint16_t vid = 0, pid = 0;
        int wireless = 0, on = cfg_bool("TOTK_HAPTICS");
        int have = on && input_pad_hid(path, sizeof path, &vid, &pid, &wireless);
        int why = !on ? 0 : !have ? 1 : !is_dualsense(vid, pid) ? 2 : wireless ? 3 : 4;   // 3 Bluetooth, 4 USB
        if (why != logged_why) {
            static const char *const msg[] = {"off", "no gamepad", "the gamepad is not a DualSense",
                                              "DualSense over Bluetooth", "DualSense over USB"};
            rt_log("haptics: %s", msg[why]);
            logged_why = why;
        }
        int mode = why >= 3 ? why : 0;
        if ((hid && (mode != 3 || strcmp(path, open_path))) || (aud && mode != 4)) {
            if (hid) SDL_hid_close(hid), hid = NULL;
            if (aud) SDL_DestroyAudioStream(aud), aud = NULL;
            plat_xchg32(&ready, 0);
        }
        if (!mode) { plat_sleep_ms(250); continue; }
        if (!hid && !aud) {
            if (mode == 3 && !(hid = SDL_hid_open_path(path))) {
                if (!open_failed++) rt_log("haptics: cannot open the DualSense (%s); SDL rumble stays", SDL_GetError());
            }
            if (mode == 4) aud = usb_open();
            if (!hid && !aud) { plat_sleep_ms(2000); continue; }
            snprintf(open_path, sizeof open_path, "%s", path);
            memset(osc, 0, sizeof osc), quiet = 0, next = 0, open_failed = 0, first_write = first_loud = 1;
            rt_log("haptics: streaming");
            plat_xchg32(&ready, 1);
            input_rumble(0, 0, 0);   // stop what SDL's emulation was playing
        }
        uint64_t now = plat_now_ns(), period = hid ? BT_PERIOD_NS : USB_PERIOD_NS;
        if (!next) next = now;
        if (now < next) plat_sleep_ns(next - now), now = plat_now_ns();
        next += period;
        if (next < now) next = now;   // late: no catch-up burst

        Side cur[2];
        plat_mutex_lock(&lock);
        memcpy(cur, want, sizeof cur);
        plat_mutex_unlock(&lock);
        uint64_t know = kern_now_ns();
        double pct = cfg_double("TOTK_RUMBLE_STRENGTH");   // the rumble strength, linear here
        float gain = (float)(pct > 0 ? (pct < 300 ? pct : 300) : 0) / 100;
        double cv = cfg_double("TOTK_RUMBLE_CURVE");
        float curve = (float)(cv > 0.2 ? (cv < 1.5 ? cv : 1.5) : 0.2);

        int n = hid ? BT_FRAMES : USB_FRAMES;
        if (aud) {   // keep about USB_QUEUE_MS queued; the period just paces the checks
            int queued = SDL_GetAudioStreamQueued(aud) / (int)(4 * sizeof(float));
            if (queued > USB_RATE * USB_QUEUE_MS / 1000) continue;
        }
        int loud = synth(osc, cur, know, gain, curve, hid ? BT_RATE : (float)USB_RATE, n, pcm);
        if (loud && first_loud) {
            float peak = 0;
            for (int i = 0; i < n * 2; i++) peak = fabsf(pcm[i]) > peak ? fabsf(pcm[i]) : peak;
            rt_log("haptics: first game vibration, peak %.2f, left %.2f@%.0f %.2f@%.0f Hz, right %.2f@%.0f %.2f@%.0f Hz",
                   (double)peak, (double)cur[0].amp_low, (double)cur[0].freq_low, (double)cur[0].amp_high,
                   (double)cur[0].freq_high, (double)cur[1].amp_low, (double)cur[1].freq_low, (double)cur[1].amp_high,
                   (double)cur[1].freq_high);
            first_loud = 0;
        }
        if (!loud && quiet == 3 && sent && quiet_logs < 5)
            rt_log("haptics: quiet after %llu blocks", (unsigned long long)sent), quiet_logs++;
        quiet = loud ? 0 : quiet + 1;
        if (quiet > 3) continue;   // silent: a few zero blocks let the actuators stop, then nothing is sent

        int ok;
        if (hid) {
            uint8_t r[REPORT_LEN];
            build(r, seq, counter, pcm);
            int w = SDL_hid_write(hid, r, sizeof r);
            if (first_write) rt_log("haptics: first report written, %d bytes", w), first_write = 0;
            ok = w >= 0;
            seq = (seq + 1) & 15, counter = (counter + 1) & 255;
        } else {
            for (int i = 0; i < n; i++)   // front left / right (speaker) silent; back left / right: the actuators
                quad[i * 4] = quad[i * 4 + 1] = 0, quad[i * 4 + 2] = pcm[i * 2], quad[i * 4 + 3] = pcm[i * 2 + 1];
            ok = SDL_PutAudioStreamData(aud, quad, n * 4 * (int)sizeof(float));
            if (first_write) rt_log("haptics: first audio block queued (%s)", ok ? "ok" : SDL_GetError()), first_write = 0;
        }
        if (!ok) {
            rt_log("haptics: send failed (%s); back to SDL rumble", SDL_GetError());
            if (hid) SDL_hid_close(hid), hid = NULL;
            if (aud) SDL_DestroyAudioStream(aud), aud = NULL;
            plat_xchg32(&ready, 0);
            plat_sleep_ms(2000);
            continue;
        }
        sent++;
    }
}

void ds5_haptics_start(void) {
    if (plat_xchg32(&started, 1)) return;
    if (!plat_thread_spawn(stream, NULL, "ds5-haptics", 0)) rt_log("haptics: no thread");
}
