#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define HAVE_STDARG_H

#include "cpu.h"
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/dma.h>
#include <86box/filters.h>
#include <86box/io.h>
#include <86box/midi.h>
#include <86box/pic.h>
#include <86box/timer.h>
#include <86box/pit.h>
#include <86box/snd_mpu401.h>
#include <86box/sound.h>
#include <86box/snd_opl.h>
#include <86box/snd_sb.h>
#include <86box/snd_sb_dsp.h>
#include <86box/plat_unused.h>

/*      Original PAS uses
                2 x OPL2
                PIT - sample rate/count
                LMC835N/LMC1982 - mixer
                YM3802 - MIDI Control System


        9A01 - IO base
                base >> 2

        All below + IO base

        B89 - interrupt status / clear
                bit 2 - sample rate
                bit 3 - PCM
                bit 4 - MIDI

        B88 - Audio mixer control register

        B8A - Audio filter control
                bit 5 - mute?

        B8B - interrupt mask / board ID
                bits 5-7 - board ID (read only on PAS16)

        F88 - PCM data (low)

        F89 - PCM data (high)

        F8A - PCM control?
                bit 4 - input/output select (1 = output)
                bit 5 - mono/stereo select
                bit 6 - PCM enable

        1388-138b - PIT clocked at 1193180 Hz
                1388 - sample rate
                1389 - sample count

        178b -
        2789 - board revision

        8389 -
                bit 2 - 8/16 bit

        BF88 - wait states

        EF8B -
                bit 3 - 16 bits okay ?

        F388 -
                bit 6 - joystick enable

        F389 -
                bits 0-2 - DMA

        F38A -
                bits 0-3 - IRQ

        F788 -
                bit 1 - SB emulation
                bit 0 - MPU401 emulation

        F789 - SB base addr
                bits 0-3 - addr bits 4-7

        FB8A - SB IRQ/DMA
                bits 3-5 - IRQ
                bits 6-7 - DMA

        FF88 - board model
                3 = PAS16
*/

typedef struct pas16_t {
    uint16_t base;

    int irq;
    int dma;

    uint8_t audiofilt;

    uint8_t audio_mixer;

    uint8_t compat;
    uint8_t compat_base;

    uint8_t enhancedscsi;

    uint8_t io_conf_1;
    uint8_t io_conf_2;
    uint8_t io_conf_3;
    uint8_t io_conf_4;

    uint8_t irq_stat;
    uint8_t irq_ena;

    uint8_t  pcm_ctrl;
    uint16_t pcm_dat;

    uint16_t pcm_dat_l;
    uint16_t pcm_dat_r;

    uint8_t sb_irqdma;

    int stereo_lr;

    uint8_t sys_conf_1;
    uint8_t sys_conf_2;
    uint8_t sys_conf_3;
    uint8_t sys_conf_4;
    uint8_t waitstates;
    uint8_t midi_ctrl;
    uint8_t midi_stat;
    uint8_t midi_data;
    uint8_t midi_fifo[16];

    fm_drv_t opl;
    sb_dsp_t dsp;
    mpu_t *mpu;

    int16_t pcm_buffer[2][SOUNDBUFLEN];

    int pos;

    int midi_uart_out;

    pit_t *pit;
} pas16_t;

static void    pas16_update(pas16_t *pas16);

static int pas16_dmas[8]    = { 4, 1, 2, 3, 0, 5, 6, 7 };
static int pas16_irqs[16]   = { 0, 2, 3, 4, 5, 6, 7, 10, 11, 12, 14, 15, 0, 0, 0, 0 };
static int pas16_sb_irqs[8] = { 0, 2, 3, 5, 7, 10, 11, 12 };
static int pas16_sb_dmas[8] = { 0, 1, 2, 3 };

enum {
    PAS16_INT_SAMP = 0x04,
    PAS16_INT_PCM  = 0x08,
    PAS16_INT_MIDI = 0x10,
};

enum {
    PAS16_PCM_MONO = 0x20,
    PAS16_PCM_ENA  = 0x40
};

enum {
    PAS16_SC2_16BIT  = 0x04,
    PAS16_SC2_MSBINV = 0x10
};

enum {
    PAS16_FILT_MUTE = 0x20
};

#ifdef ENABLE_PAS16_LOG
int pas16_do_log = ENABLE_PAS16_LOG;

static void
pas16_log(const char *fmt, ...)
{
    va_list ap;

    if (pas16_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define pas16_log(fmt, ...)
#endif

static void
pas16_update_midi_irqs(pas16_t *pas16)
{
    int irq = 0;

    pas16->irq_stat &= ~PAS16_INT_MIDI;

    if ((pas16->uart_status & 0x18) || (dev->uart_status & 0x04)) {
        pas16->irq_stat |= PAS16_INT_MIDI;
        irq = 1;
    }

    if (irq)
        picint(1 << pas16->irq);
    else
        picintc(1 << pas16->irq);
}

static void
pas16_update_tx_irq(pas16_t *pas16)
{
    pas16->uart_status &= ~0x18;

    if ((pas16->irq_ena & PAS16_INT_MIDI) && (pas16->uart_ctrl & 0x18))
        pas16->uart_status |= 0x18;

    pas16_update_midi_irqs(pas16);
}

static void
pas16_update_rx_irq(pas16_t *pas16)
{
    pas16->uart_status &= ~0x04;

    if ((pas16->irq_ena & PAS16_INT_MIDI) && (pas16->uart_ctrl & 0x04))
        pas16->uart_status |= 0x04;

    pas16_update_midi_irqs(pas16);
}

static void
pas16_scan_fifo(pas16_t *pas16)
{
    if (pas16->read_fifo_pos != pas16->write_fifo_pos) {
        pas16->uart_data     = pas16->uart_fifo[pas16->read_fifo_pos];
        pas16->read_fifo_pos = (pas16->read_fifo_pos + 1) & 0x0f;

        es1371_set_rx_irq(pas16, 1);
    } else
        es1371_set_rx_irq(pas16, 0);
}

static void
es1371_write_fifo(es1371_t *dev, uint8_t val)
{
    if (dev->write_fifo_pos < 16) {
        dev->uart_fifo[dev->write_fifo_pos] = val | UART_FIFO_BYTE_VALID;
        dev->write_fifo_pos                 = (dev->write_fifo_pos + 1) & 0x0f;
    }
}

static void
es1371_reset_fifo(es1371_t *dev)
{
    for (uint8_t i = 0; i < 16; i++)
        dev->uart_fifo[i] = 0x00000000;

    dev->read_fifo_pos = dev->write_fifo_pos = 0;

    es1371_set_rx_irq(dev, 0);
}

static void
pas16_reset(void *priv)
{
    pas16_t *pas16 = (pas16_t *) priv;

    pas16->uart_status = 0xff;
    pas16->uart_ctrl = 0x00;

    for (uint8_t i = 0; i < 16; i++)
        pas16->uart_fifo[i] = 0x00;

    pas16_set_tx_irq(pas16, 0);

    pas16_reset_fifo(pas16);

    pas16_update_midi_irqs(pas16);
}

static uint8_t
pas16_in(uint16_t port, void *priv)
{
    pas16_t *pas16 = (pas16_t *) priv;
    uint8_t  temp  = 0xff;
    switch ((port - pas16->base) + 0x388) {
        case 0x388:
        case 0x389:
        case 0x38a:
        case 0x38b:
            temp = pas16->opl.read((port - pas16->base) + 0x388, pas16->opl.priv);
            break;

        case 0xb88:
            temp = pas16->audio_mixer;
            break;

        case 0xb89:
            temp = pas16->irq_stat;
            break;

        case 0xb8a:
            temp = pas16->audiofilt;
            break;

        case 0xb8b:
            temp = pas16->irq_ena & ~0xe0;
            temp |= 0x01;
            break;

        case 0xf8a:
            temp = pas16->pcm_ctrl;
            break;

        case 0x1789:
            temp = 0;
            break;
        case 0x178a:
            temp = pas16->uart_data;
            pas16_set_rx_irq(pas16, 0);
            break;

        case 0x1b88:
            temp = pas16->uart_status;
            break;

        case 0x2789: /*Board revision*/
            temp = 0;
            break;

        case 0x7f89:
            temp = pas16->enhancedscsi & ~0x01;
            break;

        case 0x8388:
            temp = pas16->sys_conf_1;
            break;
        case 0x8389:
            temp = pas16->sys_conf_2;
            break;
        case 0x838b:
            temp = pas16->sys_conf_3;
            break;
        case 0x838c:
            temp = pas16->sys_conf_4;
            break;

        case 0xbf88:
            temp = pas16->waitstates;
            break;

        case 0xef8b:
            temp = 0x0c;
            break;

        case 0xf388:
            temp = pas16->io_conf_1;
            break;
        case 0xf389:
            temp = pas16->io_conf_2;
            break;
        case 0xf38a:
            temp = pas16->io_conf_3;
            break;
        case 0xf38b:
            temp = pas16->io_conf_4;
            break;

        case 0xf788:
            temp = pas16->compat;
            break;
        case 0xf789:
            temp = pas16->compat_base;
            break;

        case 0xfb8a:
            temp = pas16->sb_irqdma;
            break;

        case 0xff88:  /*Board model*/
            temp = 0x04; /*PAS16*/
            break;
        case 0xff8b:                   /*Master mode read*/
            temp = 0x20 | 0x10 | 0x01; /*AT bus, XT/AT timing*/
            break;

        default:
            break;
    }
    pclog("pas16_in : port %04X return %02X  %04X:%04X\n", port, temp, CS, cpu_state.pc);
    return temp;
}

static void
pas16_out(uint16_t port, uint8_t val, void *priv)
{
    pas16_t *pas16 = (pas16_t *) priv;
    pit_t *pit = (pit_t *) pas16->pit;
    pclog("pas16_out : port %04X val %02X  %04X:%04X\n", port, val, CS, cpu_state.pc);
    switch ((port - pas16->base) + 0x388) {
        case 0x388:
        case 0x389:
        case 0x38a:
        case 0x38b:
            pas16->opl.write((port - pas16->base) + 0x388, val, pas16->opl.priv);
            break;

        case 0xb88:
            pas16->audio_mixer = val;
            break;

        case 0xb89:
            pas16->irq_stat &= ~val;
            break;

        case 0xb8a:
            pas16_update(pas16);
            pas16->audiofilt = val;
            break;

        case 0xb8b:
            pas16->irq_ena = val;
            break;

        case 0xf88:
            pas16_update(pas16);
            pas16->pcm_dat = (pas16->pcm_dat & 0xff00) | val;
            break;
        case 0xf89:
            pas16_update(pas16);
            pas16->pcm_dat = (pas16->pcm_dat & 0x00ff) | (val << 8);
            break;
        case 0xf8a:
            if ((val & PAS16_PCM_ENA) && !(pas16->pcm_ctrl & PAS16_PCM_ENA)) /*Guess*/
                pas16->stereo_lr = 0;

            pas16->pcm_ctrl = val;
            break;

        case 0x1789:
        case 0x178b:
            pas16->uart_ctrl = val;

            if ((val & 0x60) == 0x60) {
                /* Reset TX */
                pas16_set_tx_irq(pas16, 1);

                /* Software reset */
                pas16_reset_fifo(pas16);
            } else {
                pas16_set_tx_irq(pas16, 1);

                pas16_update_tx_irq(pas16);
                pas16_update_rx_irq(pas16);
            }
            break;

        case 0x178a:
            midi_raw_out_byte(val);
            pas16_set_tx_irq(pas16, 1);
            break;

        case 0x1b88:
            pas16->uart_status = val;
            break;

        case 0x7f89:
            pas16->enhancedscsi = val;
            break;

        case 0x8388:
            if ((val & 0x80) && !(pas16->sys_conf_1 & 0x80)) {
                pclog("Reset.\n");
                pas16_reset(pas16);
            }
            pas16->sys_conf_1 = val;
            break;
        case 0x8389:
            pas16->sys_conf_2 = val;
            break;
        case 0x838a:
            pas16->sys_conf_3 = val;
            break;
        case 0x838b:
            pas16->sys_conf_4 = val;
            break;

        case 0xbf88:
            pas16->waitstates = val;
            break;

        case 0xf388:
            pas16->io_conf_1 = val;
            break;
        case 0xf389:
            pas16->io_conf_2 = val;
            pas16->dma       = pas16_dmas[val & 0x7];
            pclog("pas16_out : set PAS DMA %i\n", pas16->dma);
            break;
        case 0xf38a:
            pas16->io_conf_3 = val;
            pas16->irq       = pas16_irqs[val & 0xf];
            pclog("pas16_out : set PAS IRQ %i\n", pas16->irq);
            break;
        case 0xf38b:
            pas16->io_conf_4 = val;
            break;

        case 0xf788:
            pas16->compat = val;
            if (pas16->compat & 0x02)
                sb_dsp_setaddr(&pas16->dsp, ((pas16->compat_base & 0xf) << 4) | 0x200);
            else
                sb_dsp_setaddr(&pas16->dsp, 0);
            if (pas16->compat & 0x01)
                mpu401_change_addr(pas16->mpu, ((pas16->compat_base & 0xf0) | 0x300));
            else
                mpu401_change_addr(pas16->mpu, 0);
            break;
        case 0xf789:
            pas16->compat_base = val;
            if (pas16->compat & 0x02)
                sb_dsp_setaddr(&pas16->dsp, ((pas16->compat_base & 0xf) << 4) | 0x200);
            if (pas16->compat & 0x01)
                mpu401_change_addr(pas16->mpu, ((pas16->compat_base & 0xf0) | 0x300));
            break;

        case 0xfb8a:
            pas16->sb_irqdma = val;
            sb_dsp_setirq(&pas16->dsp, pas16_sb_irqs[(val >> 3) & 7]);
            sb_dsp_setdma8(&pas16->dsp, pas16_sb_dmas[(val >> 6) & 3]);
            pas16_log("pas16_out : set SB IRQ %i DMA %i\n", pas16_sb_irqs[(val >> 3) & 7], pas16_sb_dmas[(val >> 6) & 3]);
            break;

        default:
            pclog("pas16_out : unknown %04X\n", port);
    }
#if 0
    if (cpu_state.pc == 0x80048CF3) {
        if (output)
            fatal("here\n");
        output = 3;
    }
#endif
}


static void
pas16_scan_fifo(pas16_t *pas16)
{
    if (pas16->read_fifo_pos != pas16->write_fifo_pos) {
        pas16->uart_data     = pas16->uart_fifo[pas16->read_fifo_pos];
        pas16->read_fifo_pos = (pas16->read_fifo_pos + 1) & 7;

        pas16_set_rx_irq(pas16, 1);
    } else
        pas16_set_rx_irq(pas16, 0);
}

static uint8_t
pas16_readdma(pas16_t *pas16)
{
    return dma_channel_read(pas16->dma);
}


static void
pas16_pcm_poll(void *priv)
{
    pit_t *pit = (pit_t *)priv;
    pas16_t *pas16 = (pas16_t *) pit->dev_priv;
    uint16_t temp = 0x0000;


    pas16_update(pas16);
    if (pit->counters[0].m & 2) {
        if (pit->counters[0].l)
            timer_advance_u64(&pit->callback_timer, pit->counters[0].l * (PITCONST << 1ULL));
        else {
            timer_advance_u64(&pit->callback_timer, 0x10000 * (PITCONST << 1ULL));
        }
    }

    pas16->irq_stat |= PAS16_INT_SAMP;
    if (pas16->irq_ena & PAS16_INT_SAMP)
        picint(1 << pas16->irq);
    else
        picintc(1 << pas16->irq);

    /*Update sample rate counter*/
    pas16_log("Enable (t1) = %d.\n", pit->counters[1].enable);
    if (pit->counters[1].enable) {
        if (pas16->pcm_ctrl & PAS16_PCM_ENA) {
            if (pas16->sys_conf_2 & PAS16_SC2_16BIT) {
                temp = pas16_readdma(pas16) << 8;
                temp |= pas16_readdma(pas16);
            } else
                temp = (pas16_readdma(pas16) ^ 0x80) << 8;

            if (pas16->sys_conf_2 & PAS16_SC2_MSBINV)
                temp ^= 0x8000;
            if (pas16->pcm_ctrl & PAS16_PCM_MONO)
                pas16->pcm_dat_l = pas16->pcm_dat_r = temp;
            else {
                if (pas16->stereo_lr)
                    pas16->pcm_dat_r = temp;
                else
                    pas16->pcm_dat_l = temp;

                pas16->stereo_lr = !pas16->stereo_lr;
            }
        }
        if (pas16->sys_conf_2 & PAS16_SC2_16BIT)
            pit->counters[1].rl -= 2;
        else
            pit->counters[1].rl--;

        pas16_log("RL=%d, mode=%x.\n", pit->counters[1].rl, pit->counters[1].m & 0x03);
        if (pit->counters[1].rl == 0xffff) {
            if (pit->counters[1].m & 2) {
                if (pit->counters[1].l & 0xffff)
                    pit->counters[1].rl = pit->counters[1].l & 0xffff;
                else
                    pit->counters[1].rl = 0;
            } else {
                pit->counters[1].enable = 0;
                pit->counters[1].rl = 0;
            }

            pas16_log("New counter=%d, mode=%x.\n", pit->counters[1].rl, pit->counters[1].m & 0x03);
            pas16->irq_stat |= PAS16_INT_PCM;
            if (pas16->irq_ena & PAS16_INT_PCM) {
                pclog("pas16_pcm_poll : cause IRQ %i %02X, enable timer 1 = %x\n", pas16->irq, 1 << pas16->irq, pit->counters[1].enable);
                picint(1 << pas16->irq);
            } else
                picintc(1 << pas16->irq);
        }
    }
}

static void
pas16_pit_timer0(int new_out, int old_out, void *priv)
{
    pit_t *pit = (pit_t *)priv;
    pclog("NewOut=%d, OldOut=%d.\n", new_out, old_out);
    pit->counters[1].enable = new_out;
    pit_ctr_set_clock(&pit->counters[0], new_out, pit);
}

static void
pas16_out_base(UNUSED(uint16_t port), uint8_t val, void *priv)
{
    pas16_t *pas16 = (pas16_t *) priv;

    io_removehandler((pas16->base - 0x388) + 0x0388, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_removehandler((pas16->base - 0x388) + 0x0788, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_removehandler((pas16->base - 0x388) + 0x0b88, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_removehandler((pas16->base - 0x388) + 0x0f88, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    pit_handler(0, (pas16->base - 0x388) + 0x1388, 0x0004, pas16->pit);
    io_removehandler((pas16->base - 0x388) + 0x1788, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_removehandler((pas16->base - 0x388) + 0x1b88, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_removehandler((pas16->base - 0x388) + 0x2788, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_removehandler((pas16->base - 0x388) + 0x7f88, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_removehandler((pas16->base - 0x388) + 0x8388, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_removehandler((pas16->base - 0x388) + 0xbf88, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_removehandler((pas16->base - 0x388) + 0xe388, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_removehandler((pas16->base - 0x388) + 0xe788, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_removehandler((pas16->base - 0x388) + 0xeb88, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_removehandler((pas16->base - 0x388) + 0xef88, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_removehandler((pas16->base - 0x388) + 0xf388, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_removehandler((pas16->base - 0x388) + 0xf788, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_removehandler((pas16->base - 0x388) + 0xfb88, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_removehandler((pas16->base - 0x388) + 0xff88, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);

    pas16->base = val << 2;
    pclog("pas16_write_base : PAS16 base now at %04X\n", pas16->base);

    io_sethandler((pas16->base - 0x388) + 0x0388, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_sethandler((pas16->base - 0x388) + 0x0788, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_sethandler((pas16->base - 0x388) + 0x0b88, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_sethandler((pas16->base - 0x388) + 0x0f88, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    pit_handler(1, (pas16->base - 0x388) + 0x1388, 0x0004, pas16->pit);
    io_sethandler((pas16->base - 0x388) + 0x1788, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_sethandler((pas16->base - 0x388) + 0x1b88, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_sethandler((pas16->base - 0x388) + 0x2788, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_sethandler((pas16->base - 0x388) + 0x7f88, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_sethandler((pas16->base - 0x388) + 0x8388, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_sethandler((pas16->base - 0x388) + 0xbf88, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_sethandler((pas16->base - 0x388) + 0xe388, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_sethandler((pas16->base - 0x388) + 0xe788, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_sethandler((pas16->base - 0x388) + 0xeb88, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_sethandler((pas16->base - 0x388) + 0xef88, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_sethandler((pas16->base - 0x388) + 0xf388, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_sethandler((pas16->base - 0x388) + 0xf788, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_sethandler((pas16->base - 0x388) + 0xfb88, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
    io_sethandler((pas16->base - 0x388) + 0xff88, 0x0004, pas16_in, NULL, NULL, pas16_out, NULL, NULL, pas16);
}

static void
pas16_update(pas16_t *pas16)
{
    if (!(pas16->audiofilt & PAS16_FILT_MUTE)) {
        for (; pas16->pos < sound_pos_global; pas16->pos++) {
            pas16->pcm_buffer[0][pas16->pos] = 0;
            pas16->pcm_buffer[1][pas16->pos] = 0;
        }
    } else {
        for (; pas16->pos < sound_pos_global; pas16->pos++) {
            pas16->pcm_buffer[0][pas16->pos] = (int16_t) pas16->pcm_dat_l;
            pas16->pcm_buffer[1][pas16->pos] = (int16_t) pas16->pcm_dat_r;
        }
    }
}

void
pas16_get_buffer(int32_t *buffer, int len, void *priv)
{
    pas16_t *pas16 = (pas16_t *) priv;

    sb_dsp_update(&pas16->dsp);
    pas16_update(pas16);
    for (int c = 0; c < len * 2; c++) {
        buffer[c] += (int16_t) (sb_iir(0, c & 1, (double) pas16->dsp.buffer[c]) / 1.3) / 2;
        buffer[c] += (pas16->pcm_buffer[c & 1][c >> 1] / 2);
    }

    pas16->pos = 0;
    pas16->dsp.pos = 0;
}

void
pas16_get_music_buffer(int32_t *buffer, int len, void *priv)
{
    pas16_t *pas16 = (pas16_t *) priv;

    const int32_t *opl_buf = pas16->opl.update(pas16->opl.priv);
    for (int c = 0; c < len * 2; c++)
        buffer[c] += opl_buf[c];

    pas16->opl.reset_buffer(pas16->opl.priv);
}

static void *
pas16_init(UNUSED(const device_t *info))
{
    pas16_t *pas16 = malloc(sizeof(pas16_t));
    memset(pas16, 0, sizeof(pas16_t));

    fm_driver_get(FM_YMF262, &pas16->opl);
    sb_dsp_init(&pas16->dsp, SB2, SB_SUBTYPE_DEFAULT, pas16);
    pas16->mpu = (mpu_t *) malloc(sizeof(mpu_t));
    memset(pas16->mpu, 0, sizeof(mpu_t));
    mpu401_init(pas16->mpu, 0, 0, M_UART, device_get_config_int("receive_input401"));
    sb_dsp_set_mpu(&pas16->dsp, pas16->mpu);

    pas16->pit = device_add(&i8254_ext_io_device);

    pas16->midi_uart_out = 1;

    io_sethandler(0x9a01, 0x0001, NULL, NULL, NULL, pas16_out_base, NULL, NULL, pas16);
    pit_ctr_set_out_func(pas16->pit, 0, pas16_pit_timer0);
    pit_ctr_set_using_timer(pas16->pit, 0, 1);
    pit_ctr_set_using_timer(pas16->pit, 1, 0);
    pit_ctr_set_using_timer(pas16->pit, 2, 0);
    pas16->pit->dev_priv = pas16;
    pas16->pit->dev_timer = pas16_pcm_poll;

    sound_add_handler(pas16_get_buffer, pas16);
    music_add_handler(pas16_get_music_buffer, pas16);

    return pas16;
}

static void
pas16_close(void *priv)
{
    pas16_t *pas16 = (pas16_t *) priv;

    free(pas16);
}

static const device_config_t pas16_config[] = {
    {
        .name = "receive_input401",
        .description = "Receive input (MPU-401)",
        .type = CONFIG_BINARY,
        .default_string = "",
        .default_int = 0
    },
    { .name = "", .description = "", .type = CONFIG_END }
};

const device_t pas16_device = {
    .name          = "Pro Audio Spectrum 16",
    .internal_name = "pas16",
    .flags         = DEVICE_ISA | DEVICE_AT,
    .local         = 0,
    .init          = pas16_init,
    .close         = pas16_close,
    .reset         = NULL,
    { .available = NULL },
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = pas16_config
};
