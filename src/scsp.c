/*
 * Yamaha YMF292-F SCSP, ported to C from MAME's sound/scsp.cpp and
 * scspdsp.cpp (license:BSD-3-Clause, copyright-holders:ElSemi, R. Belmont;
 * thanks-to kingshriek). The structure follows the original closely so that
 * it can be checked against it; what differs is the framework glue: timers
 * count samples instead of scheduling events, the MIDI port hands bytes over
 * whole rather than bit by bit, and the sample memory is a plain array.
 */

#include "model2recomp/scsp.h"

#include <stdbool.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned g_scsp_keyons, g_scsp_writes, g_scsp_irqs, g_scsp_timer_fires, g_scsp_timerbc_fires, g_scsp_midi_reads, g_scsp_trace_left = 40;
unsigned g_scsp_keyon_log = 0;   /* MODEL2_KEYONLOG=N: describe the first N key-ons */

#define SHIFT       12
#define LFO_SHIFT   8
#define FIX(v)      ((uint32_t)((float)(1 << SHIFT) * (v)))
#define EG_SHIFT    16

enum { SCSP_ATTACK, SCSP_DECAY1, SCSP_DECAY2, SCSP_RELEASE };

typedef struct {
    int volume;
    int state;
    int step;
    int AR, D1R, D2R, RR;
    int DL;
    uint8_t EGHOLD;
    uint8_t LPLINK;
} scsp_eg_t;

typedef struct {
    uint16_t phase;
    uint32_t phase_step;
    int *table;
    int *scale;
} scsp_lfo_t;

typedef struct {
    union {
        uint16_t data[0x10];
        uint8_t  datab[0x20];
    } udata;
    uint8_t  Backwards;
    uint8_t  active;
    uint32_t cur_addr;
    uint32_t nxt_addr;
    uint32_t step;
    scsp_eg_t  EG;
    scsp_lfo_t PLFO;
    scsp_lfo_t ALFO;
    int      slot;
    int16_t  Prev;
} scsp_slot_t;

/* ---- the effects DSP ---- */
typedef struct {
    uint32_t RBP;
    uint32_t RBL;
    int16_t  COEF[64];
    uint16_t MADRS[32];
    uint16_t MPRO[128 * 4];
    int32_t  TEMP[128];
    int32_t  MEMS[32];
    uint32_t DEC;
    int32_t  MIXS[16];
    int16_t  EXTS[2];
    int16_t  EFREG[16];
    bool     Stopped;
    int      LastStep;
} scsp_dsp_t;

/* slot register fields */
#define KEYONEX(s)  ((s)->udata.data[0x0] & 0x1000)
#define KEYONB(s)   ((s)->udata.data[0x0] & 0x0800)
#define SBCTL(s)    (((s)->udata.data[0x0] >> 0x9) & 0x0003)
#define SSCTL(s)    (((s)->udata.data[0x0] >> 0x7) & 0x0003)
#define LPCTL(s)    (((s)->udata.data[0x0] >> 0x5) & 0x0003)
#define PCM8B(s)    ((s)->udata.data[0x0] & 0x0010)
#define SA(s)       ((((s)->udata.data[0x0] & 0xF) << 16) | ((s)->udata.data[0x1]))
#define LSA(s)      ((s)->udata.data[0x2])
#define LEA(s)      ((s)->udata.data[0x3])
#define D2R(s)      (((s)->udata.data[0x4] >> 0xB) & 0x001F)
#define D1R(s)      (((s)->udata.data[0x4] >> 0x6) & 0x001F)
#define EGHOLD(s)   ((s)->udata.data[0x4] & 0x0020)
#define AR(s)       ((s)->udata.data[0x4] & 0x001F)
#define LPSLNK(s)   ((s)->udata.data[0x5] & 0x4000)
#define KRS(s)      (((s)->udata.data[0x5] >> 0xA) & 0x000F)
#define DL(s)       (((s)->udata.data[0x5] >> 0x5) & 0x001F)
#define RR(s)       ((s)->udata.data[0x5] & 0x001F)
#define STWINH(s)   ((s)->udata.data[0x6] & 0x0200)
#define SDIR(s)     ((s)->udata.data[0x6] & 0x0100)
#define TL(s)       ((s)->udata.data[0x6] & 0x00FF)
#define MDL(s)      (((s)->udata.data[0x7] >> 0xC) & 0x000F)
#define MDXSL(s)    (((s)->udata.data[0x7] >> 0x6) & 0x003F)
#define MDYSL(s)    ((s)->udata.data[0x7] & 0x003F)
#define OCT(s)      (((s)->udata.data[0x8] >> 0xB) & 0x000F)
#define FNS(s)      ((s)->udata.data[0x8] & 0x03FF)
#define LFORE(s)    ((s)->udata.data[0x9] & 0x8000)
#define LFOF(s)     (((s)->udata.data[0x9] >> 0xA) & 0x001F)
#define PLFOWS(s)   (((s)->udata.data[0x9] >> 0x8) & 0x0003)
#define PLFOS(s)    (((s)->udata.data[0x9] >> 0x5) & 0x0007)
#define ALFOWS(s)   (((s)->udata.data[0x9] >> 0x3) & 0x0003)
#define ALFOS(s)    ((s)->udata.data[0x9] & 0x0007)
#define ISEL(s)     (((s)->udata.data[0xA] >> 0x3) & 0x000F)
#define IMXL(s)     ((s)->udata.data[0xA] & 0x0007)
#define DISDL(s)    (((s)->udata.data[0xB] >> 0xD) & 0x0007)
#define DIPAN(s)    (((s)->udata.data[0xB] >> 0x8) & 0x001F)
#define EFSDL(s)    (((s)->udata.data[0xB] >> 0x5) & 0x0007)
#define EFPAN(s)    ((s)->udata.data[0xB] & 0x001F)

/* common register fields */
#define MEM4B()     (g.udata.data[0] & 0x0200)
#define DAC18B()    (g.udata.data[0] & 0x0100)
#define MVOL()      (g.udata.data[0] & 0x000F)
#define RBL()       ((g.udata.data[1] >> 0x7) & 0x0003)
#define RBP()       (g.udata.data[1] & 0x003F)
#define SCILV0()    (g.udata.data[0x24 / 2] & 0xff)
#define SCILV1()    (g.udata.data[0x26 / 2] & 0xff)
#define SCILV2()    (g.udata.data[0x28 / 2] & 0xff)

#define SCIEX0  0
#define SCIEX1  1
#define SCIEX2  2
#define SCIMID  3
#define SCIDMA  4
#define SCIIRQ  5
#define SCITMA  6
#define SCITMB  7

static const double ARTimes[64] = {100000, 100000, 8100.0, 6900.0, 6000.0, 4800.0, 4000.0, 3400.0, 3000.0, 2400.0, 2000.0, 1700.0, 1500.0,
    1200.0, 1000.0, 860.0, 760.0, 600.0, 500.0, 430.0, 380.0, 300.0, 250.0, 220.0, 190.0, 150.0, 130.0, 110.0, 95.0,
    76.0, 63.0, 55.0, 47.0, 38.0, 31.0, 27.0, 24.0, 19.0, 15.0, 13.0, 12.0, 9.4, 7.9, 6.8, 6.0, 4.7, 3.8, 3.4, 3.0, 2.4,
    2.0, 1.8, 1.6, 1.3, 1.1, 0.93, 0.85, 0.65, 0.53, 0.44, 0.40, 0.35, 0.0, 0.0};
static const double DRTimes[64] = {100000, 100000, 118200.0, 101300.0, 88600.0, 70900.0, 59100.0, 50700.0, 44300.0, 35500.0, 29600.0, 25300.0, 22200.0, 17700.0,
    14800.0, 12700.0, 11100.0, 8900.0, 7400.0, 6300.0, 5500.0, 4400.0, 3700.0, 3200.0, 2800.0, 2200.0, 1800.0, 1600.0, 1400.0, 1100.0,
    920.0, 790.0, 690.0, 550.0, 460.0, 390.0, 340.0, 270.0, 230.0, 200.0, 170.0, 140.0, 110.0, 98.0, 85.0, 68.0, 57.0, 49.0, 43.0, 34.0,
    28.0, 25.0, 22.0, 18.0, 14.0, 12.0, 11.0, 8.5, 7.1, 6.1, 5.4, 4.3, 3.6, 3.1};

static const float SDLT[8] = {-1000000.0f, -36.0f, -30.0f, -24.0f, -18.0f, -12.0f, -6.0f, 0.0f};

static const float LFOFreq[32] = {
    0.17f, 0.19f, 0.23f, 0.27f, 0.34f, 0.39f, 0.45f, 0.55f, 0.68f, 0.78f, 0.92f, 1.10f, 1.39f, 1.60f, 1.87f, 2.27f,
    2.87f, 3.31f, 3.92f, 4.79f, 6.15f, 7.18f, 8.60f, 10.8f, 14.4f, 17.2f, 21.5f, 28.7f, 43.1f, 57.4f, 86.1f, 172.3f
};
static const float ASCALE[8] = {0.0f, 0.4f, 0.8f, 1.5f, 3.0f, 6.0f, 12.0f, 24.0f};
static const float PSCALE[8] = {0.0f, 7.0f, 13.5f, 27.0f, 55.0f, 112.0f, 230.0f, 494.0f};

/* ---- the chip ---- */
static struct {
    uint8_t *ram;
    uint32_t ram_mask;      /* ram_size - 1, a power of two */
    uint32_t ram_size;
    void (*irq_cb)(int level, int state);
    void (*midi_out_cb)(uint8_t byte);

    union {
        uint16_t data[0x30 / 2];
        uint8_t  datab[0x30];
    } udata;

    scsp_slot_t Slots[32];
    int16_t RINGBUF[128];
    uint8_t BUFPTR;

    uint32_t IrqTimA, IrqTimBC, IrqMidi, IrqCPU, IrqDMA;

    uint8_t  latched_MSLC;
    uint16_t latched_MSLC_data;
    uint8_t  MidiOutStack[32];
    uint8_t  MidiOutW, MidiOutR;
    bool     MidiInOverflow;
    uint16_t read_mask;
    bool     no_side_effects;
    uint8_t  MidiStack[32];
    uint8_t  MidiW, MidiR;

    int32_t EG_TABLE[0x400];
    int LPANTABLE[0x10000];
    int RPANTABLE[0x10000];

    int TimPris[3];
    int TimCnt[3];
    /* Samples until each timer fires; 0 = not armed. MAME's one-shot
     * emu_timers, in the chip's own clock: a period is (255 - count) * prescale
     * samples at clock/512. */
    int64_t TimLeft[3];

    struct {
        uint32_t dmea;
        uint16_t drga;
        uint16_t dtlg;
        uint8_t  dgate;
        uint8_t  ddir;
    } dma;

    uint16_t mcieb, mcipd;

    int ARTABLE[64], DRTABLE[64];

    scsp_dsp_t DSP;
    int16_t *RBUFDST;

    int PLFO_TRI[256], PLFO_SQR[256], PLFO_SAW[256], PLFO_NOI[256];
    int ALFO_TRI[256], ALFO_SQR[256], ALFO_SAW[256], ALFO_NOI[256];
    int PSCALES[8][256];
    int ASCALES[8][256];

    uint32_t rng;
} g;

static uint32_t scsp_rand(void)
{
    g.rng = g.rng * 1664525u + 1013904223u;
    return g.rng >> 8;
}

static inline int32_t sext(int32_t v, int bits)
{
    int s = 32 - bits;
    return (int32_t)((uint32_t)v << s) >> s;
}

static inline int32_t clamp32(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* ---- sample memory (big-endian, 20-bit address space) ---- */
static inline uint8_t mem_read_byte(uint32_t a)
{
    a &= 0xFFFFF;
    return a < g.ram_size ? g.ram[a] : 0;
}

static inline uint16_t mem_read_word(uint32_t a)
{
    a &= 0xFFFFE;
    if (a + 1 >= g.ram_size) return 0;
    return (uint16_t)((g.ram[a] << 8) | g.ram[a + 1]);
}

static inline void mem_write_word(uint32_t a, uint16_t v)
{
    a &= 0xFFFFE;
    if (a + 1 >= g.ram_size) return;
    g.ram[a] = (uint8_t)(v >> 8);
    g.ram[a + 1] = (uint8_t)v;
}

/* ---- DSP ---- */

static uint16_t dsp_pack(int32_t val)
{
    int sign = (val >> 23) & 1;
    uint32_t temp = (uint32_t)(val ^ (val << 1)) & 0xFFFFFF;
    int exponent = 0;
    for (int k = 0; k < 12; k++) {
        if (temp & 0x800000) break;
        temp <<= 1;
        exponent += 1;
    }
    if (exponent < 12)
        val = (int32_t)(((uint32_t)val << exponent) & 0x3FFFFF);
    else
        val = (int32_t)((uint32_t)val << 11);
    val >>= 11;
    val &= 0x7FF;
    val |= sign << 15;
    val |= exponent << 11;
    return (uint16_t)val;
}

static int32_t dsp_unpack(uint16_t val)
{
    int sign = (val >> 15) & 1;
    int exponent = (val >> 11) & 0xF;
    int mantissa = val & 0x7FF;
    int32_t uval = mantissa << 11;
    if (exponent > 11) {
        exponent = 11;
        uval |= sign << 22;
    } else {
        uval |= (sign ^ 1) << 22;
    }
    uval |= sign << 23;
    uval = (int32_t)((uint32_t)uval << 8);
    uval >>= 8;
    uval >>= exponent;
    return uval;
}

static void dsp_init(void)
{
    memset(&g.DSP, 0, sizeof g.DSP);
    g.DSP.RBL = 8 * 1024;
    g.DSP.Stopped = true;
}

static void dsp_start(void)
{
    scsp_dsp_t *d = &g.DSP;
    d->Stopped = false;
    int i;
    for (i = 127; i >= 0; --i) {
        const uint16_t *IPtr = d->MPRO + i * 4;
        if (IPtr[0] || IPtr[1] || IPtr[2] || IPtr[3]) break;
    }
    d->LastStep = i + 1;
}

static void dsp_set_sample(int32_t sample, int SEL, int MXL)
{
    (void)MXL;
    g.DSP.MIXS[SEL] += sample;
}

static void dsp_step(void)
{
    scsp_dsp_t *d = &g.DSP;
    if (d->Stopped) return;

    memset(d->EFREG, 0, sizeof d->EFREG);

    int32_t ACC = 0;
    int32_t MEMVAL = 0;
    int32_t FRC_REG = 0;
    int32_t Y_REG = 0;
    uint32_t ADRS_REG = 0;

    for (int step = 0; step < d->LastStep; ++step) {
        const uint16_t *IPtr = d->MPRO + step * 4;

        uint32_t TRA   = (IPtr[0] >>  8) & 0x7f;
        uint32_t TWT   = (IPtr[0] >>  7) & 0x01;
        uint32_t TWA   = (IPtr[0] >>  0) & 0x7f;

        uint32_t XSEL  = (IPtr[1] >> 15) & 0x01;
        uint32_t YSEL  = (IPtr[1] >> 13) & 0x03;
        uint32_t IRA   = (IPtr[1] >>  6) & 0x3f;
        uint32_t IWT   = (IPtr[1] >>  5) & 0x01;
        uint32_t IWA   = (IPtr[1] >>  0) & 0x1f;

        uint32_t TABLE = (IPtr[2] >> 15) & 0x01;
        uint32_t MWT   = (IPtr[2] >> 14) & 0x01;
        uint32_t MRD   = (IPtr[2] >> 13) & 0x01;
        uint32_t EWT   = (IPtr[2] >> 12) & 0x01;
        uint32_t EWA   = (IPtr[2] >>  8) & 0x0f;
        uint32_t ADRL  = (IPtr[2] >>  7) & 0x01;
        uint32_t FRCL  = (IPtr[2] >>  6) & 0x01;
        uint32_t SHFT  = (IPtr[2] >>  4) & 0x03;
        uint32_t YRL   = (IPtr[2] >>  3) & 0x01;
        uint32_t NEGB  = (IPtr[2] >>  2) & 0x01;
        uint32_t ZERO  = (IPtr[2] >>  1) & 0x01;
        uint32_t BSEL  = (IPtr[2] >>  0) & 0x01;

        uint32_t NOFL  = (IPtr[3] >> 15) & 0x01;
        uint32_t COEF  = (IPtr[3] >>  9) & 0x3f;
        uint32_t MASA  = (IPtr[3] >>  2) & 0x1f;
        uint32_t ADREB = (IPtr[3] >>  1) & 0x01;
        uint32_t NXADR = (IPtr[3] >>  0) & 0x01;

        int32_t INPUTS;
        if (IRA <= 0x1f)
            INPUTS = d->MEMS[IRA];
        else if (IRA <= 0x2F)
            INPUTS = d->MIXS[IRA - 0x20] << 4;
        else if (IRA <= 0x31)
            INPUTS = d->EXTS[IRA - 0x30] << 8;
        else
            return;

        INPUTS = sext(INPUTS, 24);

        if (IWT) {
            d->MEMS[IWA] = MEMVAL;
            if (IRA == IWA) INPUTS = MEMVAL;
        }

        int32_t B;
        if (!ZERO) {
            if (BSEL) B = ACC;
            else      B = sext(d->TEMP[(TRA + d->DEC) & 0x7f], 24);
            if (NEGB) B = 0 - B;
        } else {
            B = 0;
        }

        int32_t X;
        if (XSEL) X = INPUTS;
        else      X = sext(d->TEMP[(TRA + d->DEC) & 0x7f], 24);

        int32_t Y = 0;
        if (YSEL == 0)      Y = FRC_REG;
        else if (YSEL == 1) Y = d->COEF[COEF] >> 3;
        else if (YSEL == 2) Y = (Y_REG >> 11) & 0x1fff;
        else if (YSEL == 3) Y = (Y_REG >> 4) & 0x0fff;

        if (YRL) Y_REG = INPUTS;

        int32_t SHIFTED = 0;
        if (SHFT == 0)      SHIFTED = clamp32(ACC, -0x00800000, 0x007fffff);
        else if (SHFT == 1) SHIFTED = clamp32(ACC * 2, -0x00800000, 0x007fffff);
        else if (SHFT == 2) SHIFTED = sext(ACC * 2, 24);
        else if (SHFT == 3) SHIFTED = sext(ACC, 24);

        Y = sext(Y, 13);

        int64_t v = ((int64_t)X * (int64_t)Y) >> 12;
        ACC = (int32_t)(v + B);

        if (TWT) d->TEMP[(TWA + d->DEC) & 0x7f] = SHIFTED;

        if (FRCL) {
            if (SHFT == 3) FRC_REG = SHIFTED & 0x0fff;
            else           FRC_REG = (SHIFTED >> 11) & 0x1fff;
        }

        if (MRD || MWT) {
            uint32_t ADDR = d->MADRS[MASA];
            if (!TABLE) ADDR += d->DEC;
            if (ADREB)  ADDR += ADRS_REG & 0x0FFF;
            if (NXADR)  ADDR++;
            if (!TABLE) ADDR &= d->RBL - 1;
            else        ADDR &= 0xffff;
            ADDR += d->RBP << 12;
            ADDR <<= 1;
            if (MRD && (step & 1)) {
                if (NOFL) MEMVAL = mem_read_word(ADDR) << 8;
                else      MEMVAL = dsp_unpack(mem_read_word(ADDR));
            }
            if (MWT && (step & 1)) {
                if (NOFL) mem_write_word(ADDR, (uint16_t)(SHIFTED >> 8));
                else      mem_write_word(ADDR, dsp_pack(SHIFTED));
            }
        }

        if (ADRL) {
            if (SHFT == 3) ADRS_REG = (SHIFTED >> 12) & 0xfff;
            else           ADRS_REG = (uint32_t)INPUTS >> 16;
        }

        if (EWT) d->EFREG[EWA] += (int16_t)(SHIFTED >> 8);
    }
    --d->DEC;
    memset(d->MIXS, 0, sizeof d->MIXS);
}

/* ---- interrupts ---- */

static uint8_t DecodeSCI(uint8_t irq)
{
    uint8_t SCI = 0;
    SCI |= (SCILV0() & (1 << irq)) ? 1 : 0;
    SCI |= ((SCILV1() & (1 << irq)) ? 1 : 0) << 1;
    SCI |= ((SCILV2() & (1 << irq)) ? 1 : 0) << 2;
    return SCI;
}

static void irq(uint32_t level, int state)
{
    if (state) g_scsp_irqs++;
    if (g.irq_cb) g.irq_cb((int)level, state);
}

static void CheckPendingIRQ(void)
{
    uint32_t pend = g.udata.data[0x20 / 2];
    uint32_t en = g.udata.data[0x1e / 2];
    if (g.MidiW != g.MidiR) {
        g.udata.data[0x20 / 2] |= 8;
        pend |= 8;
    }
    if (!pend) return;
    if (pend & en & 0x20) { irq(g.IrqCPU, 1); return; }
    if ((pend & 0x40) && (en & 0x40))   { irq(g.IrqTimA, 1); return; }
    if ((pend & 0x80) && (en & 0x80))   { irq(g.IrqTimBC, 1); return; }
    if ((pend & 0x100) && (en & 0x100)) { irq(g.IrqTimBC, 1); return; }
    if ((pend & 8) && (en & 8))         { irq(g.IrqMidi, 1); return; }
    irq(0, 0);
}

static void MainCheckPendingIRQ(uint16_t irq_type)
{
    /* The main-CPU interrupt line is not wired on Model 2. */
    g.mcipd |= irq_type;
}

static void ResetInterrupts(void)
{
    uint32_t reset = g.udata.data[0x22 / 2];
    if (reset & 0x40)  irq(g.IrqTimA, 0);
    if (reset & 0x180) irq(g.IrqTimBC, 0);
    if (reset & 0x20) {
        g.udata.data[0x20 / 2] &= ~0x20;
        irq(g.IrqCPU, 0);
    }
    if (reset & 0x8) irq(g.IrqMidi, 0);
    CheckPendingIRQ();
}

static void timerA_cb(void)
{
    g_scsp_timer_fires++;
    g.TimCnt[0] = 0xFFFF;
    g.udata.data[0x20 / 2] |= 0x40;
    g.udata.data[0x18 / 2] &= 0xff00;
    g.udata.data[0x18 / 2] |= g.TimCnt[0] >> 8;
    CheckPendingIRQ();
    MainCheckPendingIRQ(0x40);
}

static void timerB_cb(void)
{
    g_scsp_timerbc_fires++;
    g.TimCnt[1] = 0xFFFF;
    g.udata.data[0x20 / 2] |= 0x80;
    g.udata.data[0x1a / 2] &= 0xff00;
    g.udata.data[0x1a / 2] |= g.TimCnt[1] >> 8;
    CheckPendingIRQ();
}

static void timerC_cb(void)
{
    g_scsp_timerbc_fires++;
    g.TimCnt[2] = 0xFFFF;
    g.udata.data[0x20 / 2] |= 0x100;
    g.udata.data[0x1c / 2] &= 0xff00;
    g.udata.data[0x1c / 2] |= g.TimCnt[2] >> 8;
    CheckPendingIRQ();
}

/* ---- envelope ---- */

static int Get_AR(int base, int R)
{
    int Rate = base + (R << 1);
    return g.ARTABLE[Rate < 0 ? 0 : Rate > 63 ? 63 : Rate];
}

static int Get_DR(int base, int R)
{
    int Rate = base + (R << 1);
    return g.DRTABLE[Rate < 0 ? 0 : Rate > 63 ? 63 : Rate];
}

static void Compute_EG(scsp_slot_t *slot)
{
    int octave = (OCT(slot) ^ 8) - 8;
    int rate;
    if (KRS(slot) != 0xf)
        rate = octave + 2 * KRS(slot) + ((FNS(slot) >> 9) & 1);
    else
        rate = 0;

    slot->EG.volume = 0x17F << EG_SHIFT;
    slot->EG.AR = Get_AR(rate, AR(slot));
    slot->EG.D1R = Get_DR(rate, D1R(slot));
    slot->EG.D2R = Get_DR(rate, D2R(slot));
    slot->EG.RR = Get_DR(rate, RR(slot));
    slot->EG.DL = 0x1f - DL(slot);
    slot->EG.EGHOLD = (uint8_t)(EGHOLD(slot) != 0);
}

static void StopSlot(scsp_slot_t *slot, int keyoff)
{
    if (keyoff) slot->EG.state = SCSP_RELEASE;
    else        slot->active = 0;
    slot->udata.data[0] &= ~0x800;
}

static int EG_Update(scsp_slot_t *slot)
{
    switch (slot->EG.state) {
    case SCSP_ATTACK:
        slot->EG.volume += slot->EG.AR;
        if (slot->EG.volume >= (0x3ff << EG_SHIFT)) {
            if (!LPSLNK(slot)) {
                slot->EG.state = SCSP_DECAY1;
                if (slot->EG.D1R >= (1024 << EG_SHIFT))
                    slot->EG.state = SCSP_DECAY2;
            }
            slot->EG.volume = 0x3ff << EG_SHIFT;
        }
        if (slot->EG.EGHOLD)
            return 0x3ff << (SHIFT - 10);
        break;
    case SCSP_DECAY1:
        slot->EG.volume -= slot->EG.D1R;
        if (slot->EG.volume <= 0) slot->EG.volume = 0;
        if (slot->EG.volume >> (EG_SHIFT + 5) <= slot->EG.DL)
            slot->EG.state = SCSP_DECAY2;
        break;
    case SCSP_DECAY2:
        if (D2R(slot) == 0)
            return (slot->EG.volume >> EG_SHIFT) << (SHIFT - 10);
        slot->EG.volume -= slot->EG.D2R;
        if (slot->EG.volume <= 0) slot->EG.volume = 0;
        break;
    case SCSP_RELEASE:
        slot->EG.volume -= slot->EG.RR;
        if (slot->EG.volume <= 0) {
            slot->EG.volume = 0;
            StopSlot(slot, 0);
        }
        break;
    default:
        return 1 << SHIFT;
    }
    return (slot->EG.volume >> EG_SHIFT) << (SHIFT - 10);
}

static uint32_t Step(scsp_slot_t *slot)
{
    int octave = (OCT(slot) ^ 8) - 8 + SHIFT - 10;
    uint32_t Fn = FNS(slot) + (1 << 10);
    if (octave >= 0) Fn <<= octave;
    else             Fn >>= -octave;
    return Fn;
}

/* ---- LFO ---- */

#define LFIX(v) ((uint32_t)((float)(1 << LFO_SHIFT) * (v)))
#define DB(v)   LFIX(powf(10.0f, (v) / 20.0f))
#define CENTS(v) LFIX(powf(2.0f, (v) / 1200.0f))

static void LFO_Init(void)
{
    for (int i = 0; i < 256; ++i) {
        int a, p;
        a = 255 - i;
        p = i < 128 ? i : i - 256;
        g.ALFO_SAW[i] = a;
        g.PLFO_SAW[i] = p;

        if (i < 128) { a = 255; p = 127; }
        else         { a = 0;   p = -128; }
        g.ALFO_SQR[i] = a;
        g.PLFO_SQR[i] = p;

        if (i < 128) a = 255 - (i * 2);
        else         a = (i * 2) - 256;
        if (i < 64)       p = i * 2;
        else if (i < 128) p = 255 - i * 2;
        else if (i < 192) p = 256 - i * 2;
        else              p = i * 2 - 511;
        g.ALFO_TRI[i] = a;
        g.PLFO_TRI[i] = p;

        a = (int)(scsp_rand() & 0xff);
        p = 128 - a;
        g.ALFO_NOI[i] = a;
        g.PLFO_NOI[i] = p;
    }

    for (int s = 0; s < 8; ++s) {
        float limit = PSCALE[s];
        for (int i = -128; i < 128; ++i)
            g.PSCALES[s][i + 128] = (int)CENTS((limit * (float)i) / 128.0f);
        limit = -ASCALE[s];
        for (int i = 0; i < 256; ++i)
            g.ASCALES[s][i] = (int)DB((limit * (float)i) / 256.0f);
    }
}

static int32_t PLFO_Step(scsp_lfo_t *LFO)
{
    int p;
    LFO->phase = (uint16_t)(LFO->phase + LFO->phase_step);
    p = LFO->table[LFO->phase >> LFO_SHIFT];
    p = LFO->scale[p + 128];
    return p << (SHIFT - LFO_SHIFT);
}

static int32_t ALFO_Step(scsp_lfo_t *LFO)
{
    int p;
    LFO->phase = (uint16_t)(LFO->phase + LFO->phase_step);
    p = LFO->table[LFO->phase >> LFO_SHIFT];
    p = LFO->scale[p];
    return p << (SHIFT - LFO_SHIFT);
}

static void LFO_ComputeStep(scsp_lfo_t *LFO, uint32_t LFOF_, uint32_t LFOWS, uint32_t LFOS, int ALFO)
{
    float step = (float)LFOFreq[LFOF_] * 256.0f / 44100.0f;
    LFO->phase_step = (uint32_t)((float)(1 << LFO_SHIFT) * step);
    if (ALFO) {
        switch (LFOWS) {
        case 0: LFO->table = g.ALFO_SAW; break;
        case 1: LFO->table = g.ALFO_SQR; break;
        case 2: LFO->table = g.ALFO_TRI; break;
        case 3: LFO->table = g.ALFO_NOI; break;
        }
        LFO->scale = g.ASCALES[LFOS];
    } else {
        switch (LFOWS) {
        case 0: LFO->table = g.PLFO_SAW; break;
        case 1: LFO->table = g.PLFO_SQR; break;
        case 2: LFO->table = g.PLFO_TRI; break;
        case 3: LFO->table = g.PLFO_NOI; break;
        }
        LFO->scale = g.PSCALES[LFOS];
    }
}

static void Compute_LFO(scsp_slot_t *slot)
{
    if (PLFOS(slot) != 0)
        LFO_ComputeStep(&slot->PLFO, LFOF(slot), PLFOWS(slot), PLFOS(slot), 0);
    if (ALFOS(slot) != 0)
        LFO_ComputeStep(&slot->ALFO, LFOF(slot), ALFOWS(slot), ALFOS(slot), 1);
}

static void StartSlot(scsp_slot_t *slot)
{
    g_scsp_keyons++;
    if (g_scsp_keyon_log) {
        g_scsp_keyon_log--;
        fprintf(stderr, "[scsp] keyon slot %2d:", slot->slot);
        for (int i = 0; i < 12; i++) fprintf(stderr, " %04X", slot->udata.data[i]);
        fprintf(stderr, " | SA=%05X LSA=%04X LEA=%04X 8b=%d TL=%02X DISDL=%d DIPAN=%02X EFSDL=%d IMXL=%d ISEL=%d MVOL=%d dsp=%s/%d mem[SA..]=%02X%02X %02X%02X %02X%02X\n",
                SA(slot), LSA(slot), LEA(slot), PCM8B(slot) ? 1 : 0, TL(slot), DISDL(slot), DIPAN(slot), EFSDL(slot), IMXL(slot), ISEL(slot), MVOL(),
                g.DSP.Stopped ? "stopped" : "running", g.DSP.LastStep,
                mem_read_byte(SA(slot)), mem_read_byte(SA(slot) + 1), mem_read_byte(SA(slot) + 2), mem_read_byte(SA(slot) + 3), mem_read_byte(SA(slot) + 4), mem_read_byte(SA(slot) + 5));
    }
    slot->active = 1;
    slot->cur_addr = 0;
    slot->nxt_addr = 1 << SHIFT;
    slot->step = Step(slot);
    Compute_EG(slot);
    slot->EG.state = SCSP_ATTACK;
    slot->EG.volume = 0x17F << EG_SHIFT;
    slot->Prev = 0;
    slot->Backwards = 0;
    Compute_LFO(slot);
}

/* ---- init / reset ---- */

static void scsp_core_init(void)
{
    dsp_init();

    g.IrqTimA = g.IrqTimBC = g.IrqMidi = g.IrqCPU = g.IrqDMA = 0;
    g.MidiR = g.MidiW = 0;
    g.MidiInOverflow = false;
    g.MidiOutR = g.MidiOutW = 0;
    g.read_mask = 0xffff;

    for (int i = 0; i < 0x400; ++i) {
        float envDB = ((float)(3 * (i - 0x3ff))) / 32.0f;
        float scale = (float)(1 << SHIFT);
        g.EG_TABLE[i] = (int32_t)(powf(10.0f, envDB / 20.0f) * scale);
    }

    for (int i = 0; i < 0x10000; ++i) {
        int iTL  = (i >> 0x0) & 0xff;
        int iPAN = (i >> 0x8) & 0x1f;
        int iSDL = (i >> 0xD) & 0x07;
        float TL_, SegaDB = 0.0f, fSDL, PAN, LPAN, RPAN;

        if (iTL & 0x01) SegaDB -= 0.4f;
        if (iTL & 0x02) SegaDB -= 0.8f;
        if (iTL & 0x04) SegaDB -= 1.5f;
        if (iTL & 0x08) SegaDB -= 3.0f;
        if (iTL & 0x10) SegaDB -= 6.0f;
        if (iTL & 0x20) SegaDB -= 12.0f;
        if (iTL & 0x40) SegaDB -= 24.0f;
        if (iTL & 0x80) SegaDB -= 48.0f;

        TL_ = powf(10.0f, SegaDB / 20.0f);

        SegaDB = 0;
        if (iPAN & 0x1) SegaDB -= 3.0f;
        if (iPAN & 0x2) SegaDB -= 6.0f;
        if (iPAN & 0x4) SegaDB -= 12.0f;
        if (iPAN & 0x8) SegaDB -= 24.0f;

        if ((iPAN & 0xf) == 0xf) PAN = 0.0f;
        else PAN = powf(10.0f, SegaDB / 20.0f);

        if (iPAN < 0x10) { LPAN = PAN; RPAN = 1.0f; }
        else             { RPAN = PAN; LPAN = 1.0f; }

        if (iSDL) fSDL = powf(10.0f, (SDLT[iSDL]) / 20.0f);
        else      fSDL = 0.0f;

        g.LPANTABLE[i] = (int)FIX(4.0f * LPAN * TL_ * fSDL);
        g.RPANTABLE[i] = (int)FIX(4.0f * RPAN * TL_ * fSDL);
    }

    g.ARTABLE[0] = g.DRTABLE[0] = 0;
    g.ARTABLE[1] = g.DRTABLE[1] = 0;
    for (int i = 2; i < 64; ++i) {
        double step, scale;
        double t = ARTimes[i];
        if (t != 0.0) {
            step = (1023 * 1000.0) / (44100.0 * t);
            scale = (double)(1 << EG_SHIFT);
            g.ARTABLE[i] = (int)(step * scale);
        } else {
            g.ARTABLE[i] = 1024 << EG_SHIFT;
        }
        t = DRTimes[i];
        step = (1023 * 1000.0) / (44100.0 * t);
        scale = (double)(1 << EG_SHIFT);
        g.DRTABLE[i] = (int)(step * scale);
    }

    for (int i = 0; i < 32; ++i) {
        g.Slots[i].slot = i;
        g.Slots[i].active = 0;
        g.Slots[i].EG.state = SCSP_RELEASE;
    }

    LFO_Init();
    g.udata.data[0x20 / 2] = 0;
    g.TimCnt[0] = g.TimCnt[1] = g.TimCnt[2] = 0xffff;
    g.TimLeft[0] = g.TimLeft[1] = g.TimLeft[2] = 0;
}

void scsp_init(uint8_t *ram, uint32_t ram_size,
               void (*irq_cb)(int level, int state),
               void (*midi_out_cb)(uint8_t byte))
{
    memset(&g, 0, sizeof g);
    g.ram = ram;
    g.ram_size = ram_size;
    g.irq_cb = irq_cb;
    g.midi_out_cb = midi_out_cb;
    g.rng = 0x12345678;
    scsp_core_init();
}

void scsp_reset(void)
{
    uint8_t *ram = g.ram; uint32_t size = g.ram_size;
    void (*icb)(int, int) = g.irq_cb; void (*mcb)(uint8_t) = g.midi_out_cb;
    scsp_init(ram, size, icb, mcb);
}

/* ---- registers ---- */

static void exec_dma(void);

static void UpdateSlotReg(int s, int r)
{
    scsp_slot_t *slot = g.Slots + s;
    switch (r & 0x3f) {
    case 0:
    case 1:
        if (KEYONEX(slot)) {
            for (int sl = 0; sl < 32; ++sl) {
                scsp_slot_t *s2 = g.Slots + sl;
                if (KEYONB(s2) && s2->EG.state == SCSP_RELEASE)
                    StartSlot(s2);
                if (!KEYONB(s2))
                    StopSlot(s2, 1);
            }
            slot->udata.data[0] &= ~0x1000;
        }
        break;
    case 0x10:
    case 0x11:
        slot->step = Step(slot);
        break;
    case 0xA:
    case 0xB:
        slot->EG.RR = Get_DR(0, RR(slot));
        slot->EG.DL = 0x1f - DL(slot);
        break;
    case 0x12:
    case 0x13:
        Compute_LFO(slot);
        break;
    }
}

/* Arm timer n from its register: MAME programs a one-shot for
 * (255 - count) * prescale ticks of the sample clock. */
static void arm_timer(int n, int regoff)
{
    uint16_t r = g.udata.data[regoff / 2];
    if (g_scsp_trace_left && g_scsp_keyon_log) { g_scsp_trace_left--; fprintf(stderr, "[scsp] timer %c = %04X (SCIEB %04X SCILV %02X/%02X/%02X)\n", 'A' + n, r, g.udata.data[0x1e / 2], SCILV0(), SCILV1(), SCILV2()); }
    g.TimPris[n] = 1 << ((r >> 8) & 0x7);
    g.TimCnt[n] = (r & 0xff) << 8;
    if ((r & 0xff) != 255) {
        int64_t period = (int64_t)(255 - (r & 0xff)) * g.TimPris[n];
        if (period > 0) g.TimLeft[n] = period;
    }
}

static void midi_out_flush(void)
{
    /* MAME clocks each byte out at 31250 baud; here it is delivered whole,
     * which is what the UART on the other end sees anyway. */
    while (g.MidiOutR != g.MidiOutW) {
        uint8_t b = g.MidiOutStack[g.MidiOutR];
        g.MidiOutR = (uint8_t)((g.MidiOutR + 1) & 31);
        if (g.midi_out_cb) g.midi_out_cb(b);
    }
}

static void UpdateReg(int reg)
{
    switch (reg & 0x3f) {
    case 0x0:
        break;      /* MVOL is applied at output */
    case 0x2:
    case 0x3:
        g.DSP.RBL = (8 * 1024) << RBL();
        g.DSP.RBP = RBP();
        break;
    case 0x6:
    case 0x7: {
        uint8_t data = g.udata.data[0x6 / 2] & 0xff;
        g.MidiOutStack[g.MidiOutW++] = data;
        g.MidiOutW &= 31;
        midi_out_flush();
        break;
    }
    case 8:
    case 9:
        g.latched_MSLC = (g.udata.data[0x8 / 2] & 0xf800) >> 11;
        break;
    case 0x12:
    case 0x13:
        g.dma.dmea = (g.udata.data[0x12 / 2] & 0xfffe) | (g.dma.dmea & 0xf0000);
        break;
    case 0x14:
    case 0x15:
        g.dma.dmea = ((g.udata.data[0x14 / 2] & 0xf000) << 4) | (g.dma.dmea & 0xfffe);
        g.dma.drga = (g.udata.data[0x14 / 2] & 0x0ffe);
        break;
    case 0x16:
    case 0x17:
        g.dma.dtlg = (g.udata.data[0x16 / 2] & 0x0ffe);
        g.dma.ddir = (g.udata.data[0x16 / 2] & 0x2000) >> 13;
        g.dma.dgate = (g.udata.data[0x16 / 2] & 0x4000) >> 14;
        if (g.udata.data[0x16 / 2] & 0x1000)
            exec_dma();
        break;
    case 0x18:
    case 0x19:
        arm_timer(0, 0x18);
        break;
    case 0x1a:
    case 0x1b:
        arm_timer(1, 0x1a);
        break;
    case 0x1c:
    case 0x1d:
        arm_timer(2, 0x1c);
        break;
    case 0x1e:
    case 0x1f:
        CheckPendingIRQ();
        break;
    case 0x20:
    case 0x21:
        if (g.udata.data[0x1e / 2] & g.udata.data[0x20 / 2] & 0x20)
            CheckPendingIRQ();
        break;
    case 0x22:
    case 0x23:
        g.udata.data[0x20 / 2] &= ~g.udata.data[0x22 / 2];
        ResetInterrupts();
        if (g.TimCnt[0] == 0xffff) g.udata.data[0x20 / 2] |= 0x40;
        if (g.TimCnt[1] == 0xffff) g.udata.data[0x20 / 2] |= 0x80;
        if (g.TimCnt[2] == 0xffff) g.udata.data[0x20 / 2] |= 0x100;
        break;
    case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29:
        g.IrqTimA = DecodeSCI(SCITMA);
        g.IrqTimBC = DecodeSCI(SCITMB);
        g.IrqMidi = DecodeSCI(SCIMID);
        g.IrqCPU = DecodeSCI(SCIIRQ);
        g.IrqDMA = DecodeSCI(SCIDMA);
        break;
    case 0x2a:
    case 0x2b:
        g.mcieb = g.udata.data[0x2a / 2];
        MainCheckPendingIRQ(0);
        break;
    case 0x2c:
    case 0x2d:
        if (g.udata.data[0x2c / 2] & 0x20)
            MainCheckPendingIRQ(0x20);
        break;
    case 0x2e:
    case 0x2f:
        g.mcipd &= ~g.udata.data[0x2e / 2];
        MainCheckPendingIRQ(0);
        break;
    }
}

static void UpdateRegR(int reg)
{
    switch (reg & 0x3f) {
    case 4:
    case 5: {
        uint16_t v = g.MidiStack[g.MidiR];
        if (((g.MidiOutW + 1) & 31) == g.MidiOutR) v |= 0x1000;
        if (g.MidiOutR == g.MidiOutW)              v |= 0x0800;
        if (g.MidiInOverflow)                      v |= 0x0400;
        if (((g.MidiW + 1) & 31) == g.MidiR)       v |= 0x0200;
        if (g.MidiR == g.MidiW)                    v |= 0x0100;
        g.udata.data[0x4 / 2] = v;

        if ((g.read_mask & 0x00ff) && !g.no_side_effects) {
            g.MidiInOverflow = false;
            if (g.MidiR != g.MidiW) {
                g_scsp_midi_reads++;
                ++g.MidiR;
                g.MidiR &= 31;
            }
            if (g.MidiR == g.MidiW) {
                irq(g.IrqMidi, 0);
                g.udata.data[0x20 / 2] &= ~8;
            }
        }
        break;
    }
    case 8:
    case 9:
        g.udata.data[0x8 / 2] = g.latched_MSLC_data;
        break;
    case 0x2a:
    case 0x2b:
        g.udata.data[0x2a / 2] = g.mcieb;
        break;
    case 0x2c:
    case 0x2d:
        g.udata.data[0x2c / 2] = g.mcipd;
        break;
    }
}

static inline uint16_t get16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

static void w16(uint32_t addr, uint16_t val)
{
    addr &= 0xffff;
    if (addr < 0x400) {
        int slot = addr / 0x20;
        addr &= 0x1f;
        put16(g.Slots[slot].udata.datab + addr, val);
        UpdateSlotReg(slot, addr & 0x1f);
    } else if (addr < 0x600) {
        if (addr < 0x430) {
            if (addr == 0x420 || addr == 0x42e) {
                uint8_t *p = g.udata.datab + (addr & 0x3f);
                put16(p, get16(p) | (val & 0x20));
            } else {
                put16(g.udata.datab + (addr & 0x3f), val);
            }
            UpdateReg(addr & 0x3f);
        }
    } else if (addr < 0x700) {
        g.RINGBUF[(addr - 0x600) / 2] = (int16_t)val;
    } else {
        if (addr < 0x780)
            g.DSP.COEF[(addr - 0x700) / 2] = (int16_t)val;
        else if (addr < 0x7c0)
            g.DSP.MADRS[(addr - 0x780) / 2] = val;
        else if (addr < 0x800)
            ;   /* 0x7c0-0x7ff does not mirror MADRS */
        else if (addr < 0xC00) {
            g.DSP.MPRO[(addr - 0x800) / 2] = val;
            if (addr == 0xBF0) dsp_start();
        }
    }
}

static uint16_t r16(uint32_t addr)
{
    uint16_t v = 0;
    addr &= 0xffff;
    if (addr < 0x400) {
        int slot = addr / 0x20;
        addr &= 0x1f;
        v = get16(g.Slots[slot].udata.datab + addr);
    } else if (addr < 0x600) {
        if (addr < 0x430) {
            UpdateRegR(addr & 0x3f);
            v = get16(g.udata.datab + (addr & 0x3f));
        }
    } else if (addr < 0x700) {
        v = (uint16_t)g.RINGBUF[(addr - 0x600) / 2];
    } else {
        if (addr < 0x780)       v = (uint16_t)g.DSP.COEF[(addr - 0x700) / 2];
        else if (addr < 0x7c0)  v = g.DSP.MADRS[(addr - 0x780) / 2];
        else if (addr < 0x800)  v = g.DSP.MADRS[(addr - 0x7c0) / 2];
        else if (addr < 0xC00)  v = g.DSP.MPRO[(addr - 0x800) / 2];
        else if (addr < 0xE00) {
            if (addr & 2) v = (uint16_t)(g.DSP.TEMP[(addr >> 2) & 0x7f] & 0xffff);
            else          v = (uint16_t)(g.DSP.TEMP[(addr >> 2) & 0x7f] >> 16);
        } else if (addr < 0xE80) {
            if (addr & 2) v = (uint16_t)(g.DSP.MEMS[(addr >> 2) & 0x1f] & 0xffff);
            else          v = (uint16_t)(g.DSP.MEMS[(addr >> 2) & 0x1f] >> 16);
        } else if (addr < 0xEC0) {
            if (addr & 2) v = (uint16_t)(g.DSP.MIXS[(addr >> 2) & 0xf] & 0xffff);
            else          v = (uint16_t)(g.DSP.MIXS[(addr >> 2) & 0xf] >> 16);
        } else if (addr < 0xEE0) {
            v = (uint16_t)g.DSP.EFREG[(addr - 0xec0) / 2];
        } else if (addr < 0xEE4) {
            v = (uint16_t)g.DSP.EXTS[(addr - 0xee0) / 2];
        }
    }
    return v;
}

uint16_t scsp_read16(uint32_t offset, uint16_t mem_mask)
{
    g.read_mask = mem_mask;
    uint16_t data = r16(offset & 0xffe);
    g.read_mask = 0xffff;
    return data;
}

unsigned g_scsp_log_left = 0, g_scsp_log_skip = 0;
extern unsigned sound_68k_pc(void);
void scsp_write16(uint32_t offset, uint16_t data, uint16_t mem_mask)
{
    g_scsp_writes++;
    if (g_scsp_log_left) {
        if (g_scsp_log_skip) g_scsp_log_skip--;
        else { g_scsp_log_left--; fprintf(stderr, "[scsp] w %03X = %04X/%04X pc=%06X\n", offset & 0xffe, data, mem_mask, sound_68k_pc()); }
    }
    uint16_t tmp;
    g.no_side_effects = true;
    tmp = r16(offset & 0xffe);
    g.no_side_effects = false;
    tmp = (uint16_t)((tmp & ~mem_mask) | (data & mem_mask));
    w16(offset & 0xffe, tmp);
}

int scsp_midi_in_pending(void)
{
    return (g.MidiW - g.MidiR) & 31;
}

void scsp_midi_in(uint8_t byte)
{
    if (((g.MidiW + 1) & 31) == g.MidiR) {
        g.MidiInOverflow = true;
    } else {
        g.MidiStack[g.MidiW++] = byte;
        g.MidiW &= 31;
    }
    CheckPendingIRQ();
}

/* ---- DMA ---- */

static void exec_dma(void)
{
    uint16_t tmp_dma[3];
    int i;

    if (!g.dma.ddir)
        for (i = 0; i < 3; i++)
            tmp_dma[i] = g.udata.data[(0x12 + i * 2) / 2];

    if (g.dma.ddir) {
        if (g.dma.dgate) {
            for (i = 0; i < g.dma.dtlg; i += 2) {
                mem_write_word(g.dma.dmea, 0);
                g.dma.dmea += 2;
            }
        } else {
            for (i = 0; i < g.dma.dtlg; i += 2) {
                uint16_t tmp = r16(g.dma.drga);
                mem_write_word(g.dma.dmea, tmp);
                g.dma.dmea += 2;
                g.dma.drga += 2;
            }
        }
    } else {
        if (g.dma.dgate) {
            for (i = 0; i < g.dma.dtlg; i += 2) {
                w16(g.dma.drga, 0);
                g.dma.drga += 2;
            }
        } else {
            for (i = 0; i < g.dma.dtlg; i += 2) {
                uint16_t tmp = mem_read_word(g.dma.dmea);
                w16(g.dma.drga, tmp);
                g.dma.dmea += 2;
                g.dma.drga += 2;
            }
        }
    }

    if (!g.dma.ddir)
        for (i = 0; i < 3; i++)
            g.udata.data[(0x12 + i * 2) / 2] = tmp_dma[i];

    g.udata.data[0x16 / 2] &= ~0x1000;
    if (g.udata.data[0x1e / 2] & 0x10) {
        irq(g.IrqDMA, 1);
        irq(g.IrqDMA, 0);
    }
}

/* ---- rendering ---- */

static inline int32_t UpdateSlot(scsp_slot_t *slot)
{
    if (SSCTL(slot) == 3)
        return 0;

    int32_t sample = 0;
    int step = (int)slot->step;
    uint32_t addr1, addr2, addr_select;
    uint32_t *addr[2]      = {&addr1, &addr2};
    uint32_t *slot_addr[2] = {&slot->cur_addr, &slot->nxt_addr};

    if (PLFOS(slot) != 0) {
        step = step * PLFO_Step(&slot->PLFO);
        step >>= SHIFT;
    }

    if (PCM8B(slot)) {
        addr1 = slot->cur_addr >> SHIFT;
        addr2 = slot->nxt_addr >> SHIFT;
    } else {
        addr1 = (slot->cur_addr >> (SHIFT - 1)) & ~1u;
        addr2 = (slot->nxt_addr >> (SHIFT - 1)) & ~1u;
    }

    if (MDL(slot) != 0 || MDXSL(slot) != 0 || MDYSL(slot) != 0) {
        int32_t smp = (g.RINGBUF[(g.BUFPTR + MDXSL(slot)) & 63] + g.RINGBUF[(g.BUFPTR + MDYSL(slot)) & 63]) / 2;
        smp <<= 0xA;
        smp >>= 0x1A - MDL(slot);
        if (!PCM8B(slot)) smp <<= 1;
        addr1 += smp; addr2 += smp;
    }

    if (SSCTL(slot) == 0) {
        if (PCM8B(slot)) {
            int8_t p1 = (int8_t)mem_read_byte(SA(slot) + addr1);
            int8_t p2 = (int8_t)mem_read_byte(SA(slot) + addr2);
            int32_t fpart = slot->cur_addr & ((1 << SHIFT) - 1);
            int32_t s = (int)(p1 << 8) * ((1 << SHIFT) - fpart) + (int)(p2 << 8) * fpart;
            sample = s >> SHIFT;
        } else {
            int16_t p1 = (int16_t)mem_read_word(SA(slot) + addr1);
            int16_t p2 = (int16_t)mem_read_word(SA(slot) + addr2);
            int32_t fpart = slot->cur_addr & ((1 << SHIFT) - 1);
            int32_t s = (int)p1 * ((1 << SHIFT) - fpart) + (int)p2 * fpart;
            sample = s >> SHIFT;
        }
    } else if (SSCTL(slot) == 1) {
        sample = (int16_t)(scsp_rand() & 0xffff);
    } else {
        sample = 0;
    }

    if (SBCTL(slot) & 0x1) sample ^= 0x7FFF;
    if (SBCTL(slot) & 0x2) sample = (int16_t)(sample ^ 0x8000);

    if (slot->Backwards) slot->cur_addr -= step;
    else                 slot->cur_addr += step;
    slot->nxt_addr = slot->cur_addr + (1 << SHIFT);

    addr1 = slot->cur_addr >> SHIFT;
    addr2 = slot->nxt_addr >> SHIFT;

    if (addr1 >= LSA(slot) && !slot->Backwards) {
        if (LPSLNK(slot) && slot->EG.state == SCSP_ATTACK)
            slot->EG.state = SCSP_DECAY1;
    }

    for (addr_select = 0; addr_select < 2; addr_select++) {
        int32_t rem_addr;
        switch (LPCTL(slot)) {
        case 0:
            if (*addr[addr_select] >= LSA(slot) && *addr[addr_select] >= LEA(slot))
                StopSlot(slot, 0);
            break;
        case 1:
            if (*addr[addr_select] >= LEA(slot)) {
                rem_addr = (int32_t)(*slot_addr[addr_select] - (LEA(slot) << SHIFT));
                *slot_addr[addr_select] = (LSA(slot) << SHIFT) + rem_addr;
            }
            break;
        case 2:
            if ((*addr[addr_select] >= LSA(slot)) && !slot->Backwards) {
                rem_addr = (int32_t)(*slot_addr[addr_select] - (LSA(slot) << SHIFT));
                *slot_addr[addr_select] = (LEA(slot) << SHIFT) - rem_addr;
                slot->Backwards = 1;
            } else if ((*addr[addr_select] < LSA(slot) || (*slot_addr[addr_select] & 0x80000000)) && slot->Backwards) {
                rem_addr = (int32_t)((LSA(slot) << SHIFT) - *slot_addr[addr_select]);
                *slot_addr[addr_select] = (LEA(slot) << SHIFT) - rem_addr;
            }
            break;
        case 3:
            if (*addr[addr_select] >= LEA(slot)) {
                rem_addr = (int32_t)(*slot_addr[addr_select] - (LEA(slot) << SHIFT));
                *slot_addr[addr_select] = (LEA(slot) << SHIFT) - rem_addr;
                slot->Backwards = 1;
            } else if ((*addr[addr_select] < LSA(slot) || (*slot_addr[addr_select] & 0x80000000)) && slot->Backwards) {
                rem_addr = (int32_t)((LSA(slot) << SHIFT) - *slot_addr[addr_select]);
                *slot_addr[addr_select] = (LSA(slot) << SHIFT) + rem_addr;
                slot->Backwards = 0;
            }
            break;
        }
    }

    if (!SDIR(slot)) {
        if (ALFOS(slot) != 0) {
            sample = sample * ALFO_Step(&slot->ALFO);
            sample >>= SHIFT;
        }
        if (slot->EG.state == SCSP_ATTACK)
            sample = (sample * EG_Update(slot)) >> SHIFT;
        else
            sample = (sample * g.EG_TABLE[EG_Update(slot) >> (SHIFT - 10)]) >> SHIFT;
    }

    if (!STWINH(slot)) {
        if (!SDIR(slot)) {
            uint16_t Enc = (uint16_t)((TL(slot) << 0x0) | (0x7 << 0xd));
            *g.RBUFDST = (int16_t)((sample * g.LPANTABLE[Enc]) >> (SHIFT + 1));
        } else {
            uint16_t Enc = (uint16_t)((0 << 0x0) | (0x7 << 0xd));
            *g.RBUFDST = (int16_t)((sample * g.LPANTABLE[Enc]) >> (SHIFT + 1));
        }
    }

    return sample;
}

static void tick_timers(void)
{
    for (int n = 0; n < 3; n++) {
        if (g.TimLeft[n] > 0 && --g.TimLeft[n] == 0) {
            if (n == 0) timerA_cb();
            else if (n == 1) timerB_cb();
            else timerC_cb();
        }
    }
}

void scsp_generate(int16_t *out, int count)
{
    for (int s = 0; s < count; ++s) {
        int32_t smpl = 0, smpr = 0;

        for (int sl = 0; sl < 32; ++sl) {
            g.RBUFDST = g.RINGBUF + g.BUFPTR;
            if (g.Slots[sl].active) {
                scsp_slot_t *slot = g.Slots + sl;
                uint16_t Enc;
                int32_t sample = UpdateSlot(slot);

                uint16_t eff_tl = SDIR(slot) ? 0 : TL(slot);
                Enc = (uint16_t)((eff_tl << 0x0) | (IMXL(slot) << 0xd));
                dsp_set_sample((sample * g.LPANTABLE[Enc]) >> (SHIFT - 2), ISEL(slot), IMXL(slot));
                Enc = (uint16_t)((eff_tl << 0x0) | (DIPAN(slot) << 0x8) | (DISDL(slot) << 0xd));
                smpl += (sample * g.LPANTABLE[Enc]) >> SHIFT;
                smpr += (sample * g.RPANTABLE[Enc]) >> SHIFT;
            }
            ++g.BUFPTR;
            g.BUFPTR &= 63;
        }

        dsp_step();

        for (int i = 0; i < 16; ++i) {
            scsp_slot_t *slot = g.Slots + i;
            if (EFSDL(slot)) {
                uint16_t Enc = (uint16_t)((EFPAN(slot) << 0x8) | (EFSDL(slot) << 0xd));
                smpl += (g.DSP.EFREG[i] * g.LPANTABLE[Enc]) >> SHIFT;
                smpr += (g.DSP.EFREG[i] * g.RPANTABLE[Enc]) >> SHIFT;
            }
        }
        /* EXTS0/1 (external digital inputs) are silent on Model 2. */

        /* MAME clamps the 18-bit DAC path to +-131072 and the 16-bit one to
         * +-32768 after a >> 2, then scales each to the same full scale:
         * the two come out identical, so one path serves both. */
        smpl >>= 2; smpr >>= 2;

        /* master volume, which MAME applies as a stream gain */
        int mv = MVOL();
        smpl = smpl * mv / 15;
        smpr = smpr * mv / 15;

        out[s * 2]     = (int16_t)clamp32(smpl, -32768, 32767);
        out[s * 2 + 1] = (int16_t)clamp32(smpr, -32768, 32767);

        tick_timers();
    }

    /* slot monitor, latched once per render as MAME does per stream update */
    {
        scsp_slot_t *slot = g.Slots + g.latched_MSLC;
        uint32_t SGC = (uint32_t)(slot->EG.state) & 3;
        uint32_t CA = (slot->cur_addr >> (SHIFT + 12)) & 0xf;
        uint32_t EG = (0x1f - (slot->EG.volume >> (EG_SHIFT + 5))) & 0x1f;
        g.latched_MSLC_data = (uint16_t)((CA << 7) | (SGC << 5) | EG);
    }
}
