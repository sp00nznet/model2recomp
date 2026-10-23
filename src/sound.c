/*
 * Model 2A/2B/2C sound board: 68000 + SCSP + the UART to the i960.
 *
 * Memory map (MAME model2_snd):
 *   000000-07FFFF  RAM, shared with the SCSP as its sample memory
 *   100000-100FFF  SCSP registers
 *   400000         bank control (only matters past 8 MB of samples: never)
 *   600000-67FFFF  sound program ROM
 *   800000-FFFFFF  sample ROM
 *
 * Timing: the 68000 runs 256 cycles per output sample (11.2896 MHz against
 * 44.1 kHz), driven from sound_generate_samples in slices of a few samples so
 * that register writes land close to the samples they affect.
 */

#include "model2recomp/sound.h"
#include "model2recomp/scsp.h"
#include "model2recomp/timer.h"
#include "m68k.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOUND_RAM_SIZE   0x80000
#define SOUND_PROG_SIZE  0x80000
#define CYCLES_PER_SAMPLE 256
#define SLICE_SAMPLES    8

static uint8_t  s_ram[SOUND_RAM_SIZE];
static uint8_t *s_prog;
static uint32_t s_prog_size;
static uint8_t *s_samples;
static uint32_t s_samples_size;
static bool     s_ready;        /* a program is loaded and the 68000 has been reset */
static int      s_cycle_debt;   /* cycles the 68000 ran ahead of the audio clock */

/* MODEL2_WAV=path: every generated sample, in emulated time, as a WAV. */
static FILE    *s_wav;
static uint32_t s_wav_bytes;

/* --- UART (i8251) as the i960 sees it --- */
static uint8_t s_rx_fifo[64];
static unsigned s_rx_r, s_rx_w;
static uint8_t s_rx_last;

static int s_trace = -1;
static unsigned s_midi_out_count;
static void midi_out_cb(uint8_t byte)
{
    s_midi_out_count++;
    if (s_trace > 0 && s_midi_out_count <= 32) fprintf(stderr, "[snd] midi out %02X\n", byte);
    if (((s_rx_w + 1) & 63) != s_rx_r) {
        s_rx_fifo[s_rx_w] = byte;
        s_rx_w = (s_rx_w + 1) & 63;
    }
    irq_raise(IRQ_SOUND);           /* RxRDY (model2_state::sound_ready_w) */
}

static unsigned s_int_taken[8];
int sound_68k_int_ack(int level)
{
    if (level >= 0 && level < 8) s_int_taken[level]++;
    return M68K_INT_ACK_AUTOVECTOR;
}

static unsigned s_irq_log = 12;
static void scsp_irq_cb(int level, int state)
{
    if (s_irq_log && s_trace > 0 && state) { s_irq_log--; fprintf(stderr, "[snd] irq level %d state %d pc=%06X sr=%04X\n", level, state, m68k_get_reg(NULL, M68K_REG_PC), m68k_get_reg(NULL, M68K_REG_SR)); }
    /* MAME clears "line 0" to drop everything it had asserted; the 68000 has
     * no level 0, so that is a no-op there too. */
    if (level >= 1 && level <= 7)
        m68k_set_virq((unsigned)level, state ? 1 : 0);
}

/* --- 68000 bus --- */

static inline uint8_t rd8(uint32_t a)
{
    a &= 0xFFFFFF;
    if (a < SOUND_RAM_SIZE) return s_ram[a];
    if (a >= 0x100000 && a < 0x101000) {
        uint16_t v = scsp_read16(a & 0xFFE, (a & 1) ? 0x00FF : 0xFF00);
        return (a & 1) ? (uint8_t)v : (uint8_t)(v >> 8);
    }
    if (a >= 0x600000 && a < 0x600000 + SOUND_PROG_SIZE)
        return (s_prog && a - 0x600000 < s_prog_size) ? s_prog[a - 0x600000] : 0;
    if (a >= 0x800000)
        return (s_samples && a - 0x800000 < s_samples_size) ? s_samples[a - 0x800000] : 0;
    return 0;
}

static inline uint16_t rd16(uint32_t a)
{
    a &= 0xFFFFFE;
    if (a < SOUND_RAM_SIZE) return (uint16_t)((s_ram[a] << 8) | s_ram[a + 1]);
    if (a >= 0x100000 && a < 0x101000) return scsp_read16(a & 0xFFE, 0xFFFF);
    if (a >= 0x600000 && a < 0x600000 + SOUND_PROG_SIZE) {
        uint32_t o = a - 0x600000;
        return (s_prog && o + 1 < s_prog_size) ? (uint16_t)((s_prog[o] << 8) | s_prog[o + 1]) : 0;
    }
    if (a >= 0x800000) {
        uint32_t o = a - 0x800000;
        return (s_samples && o + 1 < s_samples_size) ? (uint16_t)((s_samples[o] << 8) | s_samples[o + 1]) : 0;
    }
    return 0;
}

/* MODEL2_SCSPWLOG=path: every 68000 write to the SCSP window, stamped in
 * seconds of audio generated, in the format tools/scsp replay reads. */
static FILE *s_wlog;
static long s_samples_total;
static void wlog(uint32_t off, uint16_t data, uint16_t mask)
{
    static int checked;
    if (!checked) { checked = 1; const char *e = getenv("MODEL2_SCSPWLOG"); if (e) s_wlog = fopen(e, "w"); }
    if (s_wlog) fprintf(s_wlog, "%.6f %03X %04X %04X\n", s_samples_total / 44100.0, off, data, mask);
}

static inline void wr8(uint32_t a, uint8_t v)
{
    a &= 0xFFFFFF;
    if (a < SOUND_RAM_SIZE) { s_ram[a] = v; return; }
    if (a >= 0x100000 && a < 0x101000) {
        wlog(a & 0xFFE, (a & 1) ? v : (uint16_t)(v << 8), (a & 1) ? 0x00FF : 0xFF00);
        scsp_write16(a & 0xFFE, (a & 1) ? v : (uint16_t)(v << 8), (a & 1) ? 0x00FF : 0xFF00);
        return;
    }
    /* 0x400000 bank control and ROM: nothing to do */
}

static inline void wr16(uint32_t a, uint16_t v)
{
    a &= 0xFFFFFE;
    if (a < SOUND_RAM_SIZE) { s_ram[a] = (uint8_t)(v >> 8); s_ram[a + 1] = (uint8_t)v; return; }
    if (a >= 0x100000 && a < 0x101000) { wlog(a & 0xFFE, v, 0xFFFF); scsp_write16(a & 0xFFE, v, 0xFFFF); return; }
}

unsigned int m68k_read_memory_8(unsigned int address)  { return rd8(address); }
unsigned int m68k_read_memory_16(unsigned int address) { return rd16(address); }
unsigned int m68k_read_memory_32(unsigned int address)
{
    return ((unsigned int)rd16(address) << 16) | rd16(address + 2);
}
void m68k_write_memory_8(unsigned int address, unsigned int value)  { wr8(address, (uint8_t)value); }
void m68k_write_memory_16(unsigned int address, unsigned int value) { wr16(address, (uint16_t)value); }
void m68k_write_memory_32(unsigned int address, unsigned int value)
{
    wr16(address, (uint16_t)(value >> 16));
    wr16(address + 2, (uint16_t)value);
}

/* --- board --- */

void sound_init(void)
{
    memset(s_ram, 0, sizeof s_ram);
    s_rx_r = s_rx_w = 0;
    s_rx_last = 0;
    s_cycle_debt = 0;
    s_ready = false;

    { const char *e = getenv("MODEL2_SNDTRACE"); s_trace = e ? atoi(e) : 0; }
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    scsp_init(s_ram, SOUND_RAM_SIZE, scsp_irq_cb, midi_out_cb);

    const char *wav = getenv("MODEL2_WAV");
    if (wav && !s_wav) {
        s_wav = fopen(wav, "wb");
        if (s_wav) {
            uint8_t hdr[44] = {0};
            fwrite(hdr, 1, sizeof hdr, s_wav);     /* patched at shutdown */
            s_wav_bytes = 0;
        }
    }
    printf("[sound] 68000 + SCSP initialized\n");
}

static void wav_close(void)
{
    if (!s_wav) return;
    uint32_t data = s_wav_bytes, riff = 36 + data;
    uint8_t h[44];
    memcpy(h, "RIFF", 4); memcpy(h + 4, &riff, 4); memcpy(h + 8, "WAVEfmt ", 8);
    uint32_t fmtlen = 16, rate = 44100, byterate = 44100 * 4;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    memcpy(h + 16, &fmtlen, 4); memcpy(h + 20, &pcm, 2); memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4); memcpy(h + 28, &byterate, 4); memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2); memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
    fseek(s_wav, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, s_wav);
    fclose(s_wav);
    s_wav = NULL;
}

void sound_shutdown(void)
{
    wav_close();
    /* MODEL2_SNDRAMDUMP=path: the sound board's RAM as the run ends. */
    const char *dump = getenv("MODEL2_SNDRAMDUMP");
    if (dump) { FILE *f = fopen(dump, "wb"); if (f) { fwrite(s_ram, 1, sizeof s_ram, f); fclose(f); } }
    if (s_wlog) { fclose(s_wlog); s_wlog = NULL; }
    free(s_prog);    s_prog = NULL;    s_prog_size = 0;
    free(s_samples); s_samples = NULL; s_samples_size = 0;
    s_ready = false;
}

void sound_load_program(const uint8_t *data, uint32_t size)
{
    free(s_prog);
    s_prog = (uint8_t *)malloc(size ? size : 1);
    if (!s_prog) { s_prog_size = 0; return; }
    memcpy(s_prog, data, size);
    s_prog_size = size;

    /* The reset vectors live in RAM on this board: the driver copies the
     * ROM's first 16 bytes there before releasing the 68000. */
    memcpy(s_ram, s_prog, size < 16 ? size : 16);
    m68k_pulse_reset();
    s_cycle_debt = 0;
    s_ready = true;
    printf("[sound] Sound program loaded (%u bytes), 68000 reset\n", size);
}

void sound_load_samples(const uint8_t *data, uint32_t size)
{
    free(s_samples);
    s_samples = (uint8_t *)malloc(size ? size : 1);
    if (!s_samples) { s_samples_size = 0; return; }
    memcpy(s_samples, data, size);
    s_samples_size = size;
    printf("[sound] Samples loaded (%u bytes)\n", size);
}

/* Run the 68000 for about `cycles`, booking the overrun against the next slice. */
static void run_68k(int cycles)
{
    int run = cycles - s_cycle_debt;
    if (run > 0) {
        int used = m68k_execute(run);
        s_cycle_debt = used - run;
    } else {
        s_cycle_debt = -run;
    }
}

unsigned sound_68k_pc(void) { return m68k_get_reg(NULL, M68K_REG_PC); }
extern unsigned g_scsp_log_left, g_scsp_log_skip;
static uint32_t s_pc_hist[0x40000];      /* 512 KB of ROM, by word */

/* MODEL2_SNDTRACE=1: one line per field with the 68000's PC and the board's
 * counters, and the first MIDI bytes the i960 sends. */
static unsigned s_midi_in_count, s_status_reads;
extern unsigned g_scsp_keyons, g_scsp_writes, g_scsp_irqs, g_scsp_timer_fires, g_scsp_timerbc_fires, g_scsp_midi_reads;

void sound_generate_samples(int16_t *buffer, int num_samples)
{
    if (s_trace < 0) {
        const char *e = getenv("MODEL2_SNDTRACE"); s_trace = e ? atoi(e) : 0;
        extern unsigned g_scsp_keyon_log; const char *k = getenv("MODEL2_KEYONLOG"); if (k) g_scsp_keyon_log = (unsigned)atoi(k);
    }
    if (s_trace) {
        static unsigned field;
        if (field == 600) {
            /* the hottest 68000 addresses, by slice samples */
            for (int k = 0; k < 12; k++) {
                uint32_t best = 0, bi = 0;
                for (uint32_t i = 0; i < 0x40000; i++) if (s_pc_hist[i] > best) { best = s_pc_hist[i]; bi = i; }
                if (!best) break;
                fprintf(stderr, "[snd] hot pc %06X x%u\n", 0x600000 + bi * 2, best);
                s_pc_hist[bi] = 0;
            }
            g_scsp_log_left = 0; g_scsp_log_skip = 0;
            fprintf(stderr, "[snd] ints taken:"); for (int l = 1; l < 8; l++) fprintf(stderr, " L%d=%u", l, s_int_taken[l]); fprintf(stderr, "\n");
            fprintf(stderr, "[snd] ram vectors 24-31:"); for (int v = 24; v < 32; v++) fprintf(stderr, " %08X", (unsigned)rd16(v * 4) << 16 | rd16(v * 4 + 2)); fprintf(stderr, "\n");
        }
        if ((field++ % 60) == 0)
            fprintf(stderr, "[snd] field %u pc=%06X sr=%04X midi_in=%u taken=%u out=%u scsp_w=%u keyon=%u irq=%u timA=%u timBC=%u\n",
                    field - 1, m68k_get_reg(NULL, M68K_REG_PC), m68k_get_reg(NULL, M68K_REG_SR),
                    s_midi_in_count, g_scsp_midi_reads, s_midi_out_count, g_scsp_writes, g_scsp_keyons, g_scsp_irqs, g_scsp_timer_fires, g_scsp_timerbc_fires);
    }
    if (!s_ready) {
        memset(buffer, 0, (size_t)num_samples * 2 * sizeof(int16_t));
        return;
    }
    int done = 0;
    while (done < num_samples) {
        int chunk = num_samples - done;
        if (chunk > SLICE_SAMPLES) chunk = SLICE_SAMPLES;
        run_68k(chunk * CYCLES_PER_SAMPLE);
        if (s_trace) {
            unsigned pc = m68k_get_reg(NULL, M68K_REG_PC);
            if (pc >= 0x600000 && pc < 0x680000) s_pc_hist[(pc - 0x600000) >> 1]++;
        }
        scsp_generate(buffer + done * 2, chunk);
        s_samples_total += chunk;
        done += chunk;
    }
    if (s_wav) {
        fwrite(buffer, sizeof(int16_t) * 2, (size_t)num_samples, s_wav);
        s_wav_bytes += (uint32_t)num_samples * 4;
    }
}

void sound_run_frame(void)
{
}

/* --- UART --- */

void uart_write(uint32_t offset, uint8_t data)
{
    if (s_trace > 0 && s_midi_in_count < 96)
        fprintf(stderr, "[snd] uart write +%u = %02X\n", offset, data);
    if (offset == 0) {
        s_midi_in_count++;
        scsp_midi_in(data);
        /* On the board the byte takes 320 us to clock out and the 68000 runs
         * meanwhile; here the i960 could fill the SCSP's 32-byte input FIFO
         * within one field, so give the 68000 that time now and settle it
         * against the audio clock later. */
        if (s_ready) {
            for (int i = 0; i < 8 && scsp_midi_in_pending() > 16; i++) {
                m68k_execute(512);
                s_cycle_debt += 512;
            }
        }
        /* TxRDY again at once: the transmitter never waits here. The game
         * enables this interrupt at boot and waits on it
         * (model2_state::sound_ready_w). */
        irq_raise(IRQ_SOUND);
    }
    /* +1: mode/command byte; the 8251 setup needs no modelling */
}

uint8_t uart_read(uint32_t offset)
{
    if (offset == 0) {
        if (s_rx_r != s_rx_w) {
            s_rx_last = s_rx_fifo[s_rx_r];
            s_rx_r = (s_rx_r + 1) & 63;
        }
        return s_rx_last;
    }
    s_status_reads++;
    /* status: TxRDY | TxEMPTY, plus RxRDY while a byte waits */
    return (uint8_t)(0x05 | (s_rx_r != s_rx_w ? 0x02 : 0x00));
}
