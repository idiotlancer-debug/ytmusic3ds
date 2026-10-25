/*
 * YT Music 3DS - cliente ligero pensado para la Old 3DS (ARM11 268 MHz, sin NEON).
 *
 * Diseno:
 *   - El servidor puente (server/server.py) busca en YouTube Music y entrega MP3 CBR.
 *   - Hilo de red (nucleo 1, "syscore")  -> descarga por HTTP a un buffer circular.
 *   - Hilo de decodificacion (nucleo 0)  -> minimp3 -> ndsp (DSP de la consola).
 *   - Hilo principal                     -> interfaz de texto, muy barata en CPU.
 *
 * Requiere: devkitARM + libctru, y minimp3.h (dominio publico) en include/.
 */
#include <3ds.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"

#define CONFIG_DIR    "sdmc:/3ds/ytmusic3ds"
#define CONFIG_FILE   CONFIG_DIR "/server.txt"

#define MAX_RESULTS   15
#define RING_SIZE     (512 * 1024)
#define PREBUFFER     (48 * 1024)          /* ~3 s a 128 kbps antes de sonar */
#define NET_CHUNK     (8 * 1024)
#define IN_BUF_SIZE   (16 * 1024)
#define NUM_WBUF      24                   /* 24 tramas ~ 0,63 s de audio en cola */
#define WBUF_SAMPLES  MINIMP3_MAX_SAMPLES_PER_FRAME /* int16 por trama (stereo) */
#define SLEEP_MS(ms)  svcSleepThread((s64)(ms) * 1000000LL)

typedef struct {
    char id[16];
    char title[64];
    char artist[48];
    int  dur;
} Track;

/* ------------------------------------------------------------------ estado */
static PrintConsole g_top, g_bot;
static Track  g_tracks[MAX_RESULTS];
static int    g_ntracks = 0, g_sel = 0, g_cur = -1;
static char   g_server[64] = "";
static char   g_err[96] = "";
static float  g_volume = 0.8f;
static bool   g_top_off = false;
static bool   g_active = false;            /* hay una pista en curso */

static u8    *g_ring;
static size_t g_rhead, g_rtail, g_rcount;
static LightLock g_rlock;

static volatile bool g_stop, g_net_done, g_dec_done, g_paused;
static volatile u32  g_dec_frames;
static volatile int  g_rate = 44100;
static char   g_play_url[192];
static Thread g_net_thr, g_dec_thr;

static ndspWaveBuf g_wbuf[NUM_WBUF];
static int16_t    *g_pcm;

static void set_err(const char *m) { snprintf(g_err, sizeof g_err, "%s", m); }

/* ------------------------------------------------------------ buffer circular */
static size_t ring_write(const u8 *d, size_t n) {
    LightLock_Lock(&g_rlock);
    size_t space = RING_SIZE - g_rcount;
    if (n > space) n = space;
    size_t first = RING_SIZE - g_rhead;
    if (first > n) first = n;
    memcpy(g_ring + g_rhead, d, first);
    memcpy(g_ring, d + first, n - first);
    g_rhead = (g_rhead + n) % RING_SIZE;
    g_rcount += n;
    LightLock_Unlock(&g_rlock);
    return n;
}

static size_t ring_read(u8 *d, size_t n) {
    LightLock_Lock(&g_rlock);
    if (n > g_rcount) n = g_rcount;
    size_t first = RING_SIZE - g_rtail;
    if (first > n) first = n;
    memcpy(d, g_ring + g_rtail, first);
    memcpy(d + first, g_ring, n - first);
    g_rtail = (g_rtail + n) % RING_SIZE;
    g_rcount -= n;
    LightLock_Unlock(&g_rlock);
    return n;
}

static void ring_reset(void) {
    LightLock_Lock(&g_rlock);
    g_rhead = g_rtail = g_rcount = 0;
    LightLock_Unlock(&g_rlock);
}

/* ------------------------------------------------------------------- HTTP */
static void url_encode(const char *s, char *out, size_t cap) {
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (; *s && o + 4 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

/* Descarga una respuesta corta (texto) completa. Devuelve bytes o -1. */
static int http_get_text(const char *url, char *out, size_t cap) {
    httpcContext ctx;
    u32 status = 0, got = 0;
    size_t total = 0;
    Result rc = httpcOpenContext(&ctx, HTTPC_METHOD_GET, url, 1);
    if (R_FAILED(rc)) return -1;
    httpcAddRequestHeaderField(&ctx, "User-Agent", "ytmusic3ds/0.1");
    rc = httpcBeginRequest(&ctx);
    if (R_SUCCEEDED(rc)) rc = httpcGetResponseStatusCodeTimeout(&ctx, &status, 30000000000ULL);
    if (R_FAILED(rc) || status != 200) {
        httpcCancelConnection(&ctx);
        httpcCloseContext(&ctx);
        return -1;
    }
    do {
        if (total + 4096 >= cap) break;
        got = 0;
        rc = httpcDownloadData(&ctx, (u8 *)out + total, 4096, &got);
        total += got;
    } while (rc == (s32)HTTPC_RESULTCODE_DOWNLOADPENDING);
    if (rc != 0) httpcCancelConnection(&ctx);
    httpcCloseContext(&ctx);
    out[total] = 0;
    return (int)total;
}

static void parse_results(char *text) {
    g_ntracks = 0;
    for (char *line = strtok(text, "\n"); line && g_ntracks < MAX_RESULTS; line = strtok(NULL, "\n")) {
        char *f[4];
        int n = 0;
        char *p = line;
        while (n < 4) {
            f[n++] = p;
            char *t = strchr(p, '\t');
            if (!t) break;
            *t = 0;
            p = t + 1;
        }
        if (n < 4) continue;
        char *cr = strchr(f[3], '\r');
        if (cr) *cr = 0;
        Track *t = &g_tracks[g_ntracks++];
        snprintf(t->id, sizeof t->id, "%s", f[0]);
        snprintf(t->title, sizeof t->title, "%s", f[1]);
        snprintf(t->artist, sizeof t->artist, "%s", f[2]);
        t->dur = atoi(f[3]);
    }
}

/* ------------------------------------------------------------ configuracion */
static bool ask_text(const char *hint, const char *initial, char *out, size_t cap) {
    SwkbdState kb;
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 60);
    swkbdSetHintText(&kb, hint);
    if (initial && initial[0]) swkbdSetInitialText(&kb, initial);
    out[0] = 0;
    SwkbdButton b = swkbdInputText(&kb, out, cap);
    return b == SWKBD_BUTTON_CONFIRM && out[0];
}

static bool load_server(void) {
    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) return false;
    bool ok = fgets(g_server, sizeof g_server, f) != NULL;
    fclose(f);
    if (!ok) return false;
    g_server[strcspn(g_server, "\r\n ")] = 0;
    return g_server[0] != 0;
}

static void save_server(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir(CONFIG_DIR, 0777);
    FILE *f = fopen(CONFIG_FILE, "w");
    if (!f) return;
    fprintf(f, "%s\n", g_server);
    fclose(f);
}

static bool ask_server(void) {
    char buf[64];
    if (!ask_text("IP:puerto del servidor (ej. 192.168.1.50:8000)", g_server, buf, sizeof buf))
        return false;
    snprintf(g_server, sizeof g_server, "%s", buf);
    save_server();
    return true;
}

/* ---------------------------------------------------------------- audio */
static void apply_volume(void) {
    float mix[12] = {0};
    mix[0] = mix[1] = g_volume;
    ndspChnSetMix(0, mix);
}

static bool all_idle(void) {
    for (int i = 0; i < NUM_WBUF; i++) {
        u8 s = *(volatile u8 *)&g_wbuf[i].status;
        if (s == NDSP_WBUF_QUEUED || s == NDSP_WBUF_PLAYING) return false;
    }
    return true;
}

static double elapsed_sec(void) {
    u32 inflight = 0;
    for (int i = 0; i < NUM_WBUF; i++) {
        u8 s = *(volatile u8 *)&g_wbuf[i].status;
        if (s == NDSP_WBUF_QUEUED || s == NDSP_WBUF_PLAYING) inflight += g_wbuf[i].nsamples;
    }
    double d = (double)g_dec_frames - (double)inflight;
    if (d < 0) d = 0;
    return d / (double)g_rate;
}

/* Hilo de red: HTTP -> buffer circular */
static void net_thread(void *arg) {
    (void)arg;
    httpcContext ctx;
    bool opened = false, finished = false;
    u32 status = 0;
    Result rc;
    u8 *tmp = malloc(NET_CHUNK);

    if (!tmp) goto end;
    rc = httpcOpenContext(&ctx, HTTPC_METHOD_GET, g_play_url, 1);
    if (R_FAILED(rc)) { set_err("No se pudo abrir la conexion"); goto end; }
    opened = true;
    httpcAddRequestHeaderField(&ctx, "User-Agent", "ytmusic3ds/0.1");
    rc = httpcBeginRequest(&ctx);
    if (R_SUCCEEDED(rc)) rc = httpcGetResponseStatusCodeTimeout(&ctx, &status, 45000000000ULL);
    if (R_FAILED(rc)) { set_err("Servidor sin respuesta"); goto end; }
    if (status != 200) { set_err("El servidor devolvio un error"); goto end; }

    do {
        u32 got = 0;
        rc = httpcDownloadData(&ctx, tmp, NET_CHUNK, &got);
        size_t off = 0;
        while (off < got && !g_stop) {
            size_t w = ring_write(tmp + off, got - off);
            off += w;
            if (!w) SLEEP_MS(10);          /* buffer lleno: esperar al decodificador */
        }
        if (rc == 0) finished = true;
        else if (rc != (s32)HTTPC_RESULTCODE_DOWNLOADPENDING) { set_err("Se corto la descarga"); break; }
    } while (!finished && !g_stop);

end:
    if (opened) {
        if (!finished) httpcCancelConnection(&ctx);  /* evita que Close espere a toda la cancion */
        httpcCloseContext(&ctx);
    }
    free(tmp);
    g_net_done = true;
}

/* Hilo de decodificacion: buffer circular -> minimp3 -> ndsp */
static void decode_thread(void *arg) {
    (void)arg;
    static mp3dec_t dec;
    mp3dec_frame_info_t info;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    u8 *in = malloc(IN_BUF_SIZE);
    size_t inlen = 0;
    int wi = 0, cur_rate = 44100;

    if (!in) { g_dec_done = true; return; }
    mp3dec_init(&dec);

    /* pre-buffer para evitar cortes al empezar */
    while (!g_stop && !g_net_done && g_rcount < PREBUFFER) SLEEP_MS(20);

    while (!g_stop) {
        if (inlen < IN_BUF_SIZE) inlen += ring_read(in + inlen, IN_BUF_SIZE - inlen);

        if (inlen < 2048 && !g_net_done) { SLEEP_MS(5); continue; }   /* faltan datos */
        if (inlen == 0) break;                                       /* fin de pista */

        int samples = mp3dec_decode_frame(&dec, in, (int)inlen, pcm, &info);
        if (info.frame_bytes == 0) {
            if (g_net_done && g_rcount == 0) break;
            SLEEP_MS(5);
            continue;
        }
        memmove(in, in + info.frame_bytes, inlen - info.frame_bytes);
        inlen -= info.frame_bytes;
        if (samples <= 0) continue;

        if (info.hz != cur_rate && info.hz > 0) {
            cur_rate = info.hz;
            g_rate = cur_rate;
            ndspChnSetRate(0, (float)cur_rate);
        }

        ndspWaveBuf *wb = &g_wbuf[wi];
        while (!g_stop) {
            u8 s = *(volatile u8 *)&wb->status;
            if (s != NDSP_WBUF_QUEUED && s != NDSP_WBUF_PLAYING) break;
            SLEEP_MS(5);
        }
        if (g_stop) break;

        int16_t *dst = g_pcm + (size_t)wi * WBUF_SAMPLES;
        if (info.channels == 2) {
            memcpy(dst, pcm, (size_t)samples * 4);
        } else {
            for (int i = 0; i < samples; i++) dst[2 * i] = dst[2 * i + 1] = pcm[i];
        }
        wb->data_pcm16 = dst;
        wb->nsamples = (u32)samples;
        wb->looping = false;
        DSP_FlushDataCache(dst, (u32)samples * 4);
        ndspChnWaveBufAdd(0, wb);
        g_dec_frames += (u32)samples;
        wi = (wi + 1) % NUM_WBUF;
    }
    free(in);
    g_dec_done = true;
}

static void stop_track(void) {
    if (g_active) {
        g_stop = true;
        if (g_net_thr) { threadJoin(g_net_thr, U64_MAX); threadFree(g_net_thr); g_net_thr = NULL; }
        if (g_dec_thr) { threadJoin(g_dec_thr, U64_MAX); threadFree(g_dec_thr); g_dec_thr = NULL; }
        g_active = false;
    }
    ndspChnReset(0);
}

static void start_track(int idx) {
    stop_track();
    g_cur = idx;
    g_stop = g_net_done = g_dec_done = g_paused = false;
    g_dec_frames = 0;
    g_rate = 44100;
    g_err[0] = 0;
    ring_reset();
    memset(g_wbuf, 0, sizeof g_wbuf);

    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, 44100.0f);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    apply_volume();

    snprintf(g_play_url, sizeof g_play_url, "http://%s/stream/%s.mp3", g_server, g_tracks[idx].id);

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_active = true;
    g_net_thr = threadCreate(net_thread, NULL, 32 * 1024, prio - 1, 1, false);   /* syscore */
    if (!g_net_thr) g_net_thr = threadCreate(net_thread, NULL, 32 * 1024, prio - 1, -1, false);
    g_dec_thr = threadCreate(decode_thread, NULL, 128 * 1024, prio - 1, -1, false);
    if (!g_net_thr || !g_dec_thr) {
        set_err("No se pudieron crear los hilos");
        stop_track();
    }
}

/* ------------------------------------------------------------- interfaz */
static void do_search(void) {
    char q[96], enc[300], url[420];
    static char resp[16384];
    if (!ask_text("Buscar cancion o artista", "", q, sizeof q)) return;
    url_encode(q, enc, sizeof enc);
    snprintf(url, sizeof url, "http://%s/search?q=%s", g_server, enc);
    int n = http_get_text(url, resp, sizeof resp);
    if (n < 0) { set_err("No se pudo conectar al servidor"); return; }
    parse_results(resp);
    g_sel = 0;
    g_err[0] = 0;
    if (g_ntracks == 0) set_err("Sin resultados");
}

static void draw_top(void) {
    consoleSelect(&g_top);
    consoleClear();
    printf("== Resultados (%d) ==\n\n", g_ntracks);
    if (!g_ntracks) printf("Pulsa X para buscar musica.\n");
    for (int i = 0; i < g_ntracks; i++) {
        printf("%c%c%-2d %-.26s - %-.14s\n", i == g_sel ? '>' : ' ',
               (g_active && i == g_cur) ? '*' : ' ', i + 1, g_tracks[i].title, g_tracks[i].artist);
    }
}

static void draw_bottom(void) {
    consoleSelect(&g_bot);
    consoleClear();
    printf("YT Music 3DS  -  %.22s\n\n", g_server);
    if (g_active && g_cur >= 0) {
        Track *t = &g_tracks[g_cur];
        int el = (int)elapsed_sec();
        int w = 30, fill = t->dur > 0 ? el * w / t->dur : 0;
        if (fill > w) fill = w;
        printf("%-.38s\n%-.38s\n\n[", t->title, t->artist);
        for (int i = 0; i < w; i++) putchar(i < fill ? '#' : '-');
        printf("]\n%02d:%02d / %02d:%02d\n", el / 60, el % 60, t->dur / 60, t->dur % 60);
        printf("%s\n\n", g_dec_frames == 0 ? "Cargando..." : (g_paused ? "En pausa" : "Reproduciendo"));
    } else {
        printf("Sin reproduccion.\n\n\n\n\n\n\n");
    }
    printf("Red: %3d%%   Vol: %3d%%\n", (int)(g_rcount * 100 / RING_SIZE), (int)(g_volume * 100 + 0.5f));
    if (g_err[0]) printf("\n! %.36s\n", g_err);
    printf("\nA play/pausa  B parar  X buscar\nL/R anterior/siguiente\n</> volumen  Y pantalla sup. on/off\nSELECT servidor  START salir\n");
}

static void play_rel(int d) {
    if (!g_ntracks) return;
    int i = (g_cur >= 0 ? g_cur : g_sel) + d;
    if (i < 0 || i >= g_ntracks) return;
    g_sel = i;
    start_track(i);
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, &g_top);
    consoleInit(GFX_BOTTOM, &g_bot);
    APT_SetAppCpuTimeLimit(30);          /* habilita el nucleo 1 en Old 3DS */
    httpcInit(0);
    gspLcdInit();
    LightLock_Init(&g_rlock);
    g_ring = malloc(RING_SIZE);
    g_pcm = linearAlloc((size_t)NUM_WBUF * WBUF_SAMPLES * sizeof(int16_t));

    bool audio_ok = R_SUCCEEDED(ndspInit());
    if (audio_ok) ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    else set_err("Falta sdmc:/3ds/dspfirm.cdc (usa DSP1)");

    bool ready = g_ring && g_pcm;
    if (ready && !load_server()) ready = ask_server();

    bool dirty = true;
    u32 frame = 0;
    while (ready && aptMainLoop()) {
        hidScanInput();
        u32 kd = hidKeysDown();
        if (kd & KEY_START) break;

        if ((kd & KEY_UP) && g_ntracks)   { g_sel = (g_sel + g_ntracks - 1) % g_ntracks; dirty = true; }
        if ((kd & KEY_DOWN) && g_ntracks) { g_sel = (g_sel + 1) % g_ntracks; dirty = true; }
        if (kd & (KEY_LEFT | KEY_RIGHT)) {
            g_volume += (kd & KEY_RIGHT) ? 0.1f : -0.1f;
            if (g_volume < 0.0f) g_volume = 0.0f;
            if (g_volume > 1.0f) g_volume = 1.0f;
            if (g_active) apply_volume();
            dirty = true;
        }
        if ((kd & KEY_A) && g_ntracks && audio_ok) {
            if (g_active && g_sel == g_cur) {
                g_paused = !g_paused;
                ndspChnSetPaused(0, g_paused);
            } else {
                start_track(g_sel);
            }
            dirty = true;
        }
        if (kd & KEY_B) { stop_track(); dirty = true; }
        if (kd & KEY_X) { do_search(); dirty = true; }
        if (kd & KEY_L) { play_rel(-1); dirty = true; }
        if (kd & KEY_R) { play_rel(+1); dirty = true; }
        if (kd & KEY_SELECT) { ask_server(); dirty = true; }
        if (kd & KEY_Y) {
            g_top_off = !g_top_off;
            if (g_top_off) GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_TOP);
            else GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_TOP);
        }

        /* fin de pista: siguiente o parar */
        if (g_active && g_dec_done && g_net_done && all_idle()) {
            if (!g_err[0] && g_cur + 1 < g_ntracks) { g_sel = g_cur + 1; start_track(g_sel); }
            else stop_track();
            dirty = true;
        }

        if (dirty) { draw_top(); draw_bottom(); dirty = false; }
        else if (++frame % 30 == 0) draw_bottom();   /* refresco 2 veces por segundo */

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    if (!ready) {
        consoleSelect(&g_bot);
        printf("\nNo se pudo iniciar. Pulsa START.\n");
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gspWaitForVBlank();
        }
    }

    stop_track();
    if (g_top_off) GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_TOP);
    if (audio_ok) ndspExit();
    if (g_pcm) linearFree(g_pcm);
    free(g_ring);
    gspLcdExit();
    httpcExit();
    gfxExit();
    return 0;
}
