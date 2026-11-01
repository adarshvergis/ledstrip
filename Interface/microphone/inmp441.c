/**
 * @file inmp441.c
 * @brief INMP441 microphone driver: ESP32-C3 I2S (RX, master, Philips) + GDMA ring.
 *
 * Clocking: I2S source = PLL_F160M. MCLK = 256 * fs, BCLK = 64 * fs (2 x 32-bit slots),
 * so BCK divider = 4 and the MCLK divider is 160 MHz / (256 fs), realised with the
 * fractional divider (integer + b/a).
 *
 * DMA: a circular list of descriptors, one per block. I2S raises an EOF every
 * block_bytes (rx_eof_num), GDMA flags IN_SUC_EOF and stores the descriptor address,
 * which identifies the completed block even if an interrupt was serviced late.
 */
#include "inmp441.h"

#include <string.h>
#include "build_config.h"
#include "hw_common.h"

#include "soc/i2s_struct.h"
#include "soc/gdma_struct.h"
#include "soc/gdma_channel.h"
#include "soc/gpio_sig_map.h"
#include "soc/interrupts.h"
#include "esp_attr.h"

#define MIC_SRC_CLK_HZ      160000000u
#define MIC_MCLK_MULT       256u
#define MIC_BCK_DIV         4u
#define MIC_SLOT_BITS       32u
#define MIC_DMA_CH          HW_GDMA_CH_MIC

#define MIC_MAX_WORDS       (AUDIO_HOP_SIZE * 2u)   /* per block, stereo */
#define MIC_MAX_BLOCKS      8u

#define GDMA_EVT_RX_SUC_EOF (1u << 1)

/* DMA buffers and descriptors must be in internal RAM (DRAM). */
static DRAM_ATTR int32_t        s_buf[MIC_MAX_BLOCKS][MIC_MAX_WORDS] __attribute__((aligned(4)));
static DRAM_ATTR hw_dma_desc_t  s_desc[MIC_MAX_BLOCKS] __attribute__((aligned(4)));

static mic_config_t     s_cfg;
static mic_block_cb_t   s_cb;
static void            *s_cb_arg;
static volatile bool    s_running;
static volatile uint32_t s_blocks_done;
static uint32_t         s_actual_rate;
static bool             s_initialised;

/* ---------------------------------------------------------------------------- */

static void IRAM_ATTR mic_dma_isr(void *arg)
{
    uint32_t st = GDMA.intr[MIC_DMA_CH].st.val;
    GDMA.intr[MIC_DMA_CH].clr.val = st;

    if (st & GDMA_EVT_RX_SUC_EOF) {
        uint32_t addr = GDMA.channel[MIC_DMA_CH].in.in_suc_eof_des_addr;
        uint32_t idx = (addr - (uint32_t)&s_desc[0]) / sizeof(hw_dma_desc_t);
        if (idx >= s_cfg.num_blocks) {
            idx = s_blocks_done % s_cfg.num_blocks;     /* should never happen */
        }
        s_blocks_done++;
        if (s_cb) {
            s_cb(idx, s_cb_arg);
        }
    }
}

/**
 * Find the fractional MCLK divider: src/mclk = integer + numerator/denominator,
 * denominator <= 63 (hardware limit of the x/y/z encoding).
 */
static void mic_calc_divider(uint32_t mclk, uint32_t *integer, uint32_t *num, uint32_t *den)
{
    *integer = MIC_SRC_CLK_HZ / mclk;
    uint32_t rem = MIC_SRC_CLK_HZ % mclk;
    *num = 0;
    *den = 1;
    if (rem == 0) {
        return;
    }
    uint64_t best_err = UINT64_MAX;
    for (uint32_t a = 2; a <= 63; a++) {
        /* b/a ~= rem/mclk  ->  b = round(rem * a / mclk) */
        uint32_t b = (uint32_t)(((uint64_t)rem * a + mclk / 2) / mclk);
        if (b == 0 || b >= a) {
            continue;
        }
        int64_t diff = (int64_t)rem * a - (int64_t)b * mclk;     /* error * a * mclk */
        uint64_t err = (uint64_t)(diff < 0 ? -diff : diff) * 64u / a;
        if (err < best_err) {
            best_err = err;
            *num = b;
            *den = a;
            if (err == 0) {
                break;
            }
        }
    }
}

static void mic_setup_clock(uint32_t fs)
{
    uint32_t mclk = fs * MIC_MCLK_MULT;
    uint32_t integer, num, den;
    mic_calc_divider(mclk, &integer, &num, &den);

    uint32_t x = 0, y = 0, z = 0, yn1 = 0;
    if (num) {
        yn1 = (num * 2u > den);
        z = yn1 ? den - num : num;
        x = den / z - 1u;
        y = den % z;
    }

    I2S0.tx_clkm_conf.clk_en = 1;           /* I2S core clock (shared TX/RX) */
    I2S0.rx_clkm_conf.rx_clk_sel = 2;       /* PLL_F160M */
    I2S0.rx_clkm_conf.rx_clk_active = 1;
    I2S0.rx_clkm_conf.mclk_sel = 1;         /* MCLK from RX clock */

    /* Workaround (see IDF i2s_ll): load dummy coefficients first. */
    typeof(I2S0.rx_clkm_div_conf) d = {};
    d.rx_clkm_div_x = 317; d.rx_clkm_div_y = 7; d.rx_clkm_div_z = 3; d.rx_clkm_div_yn1 = 0;
    HW_REG_SET_FIELD(I2S0.rx_clkm_conf, rx_clkm_div_num, 7);
    I2S0.rx_clkm_div_conf.val = d.val;

    d.val = 0;
    d.rx_clkm_div_x = x; d.rx_clkm_div_y = y; d.rx_clkm_div_z = z; d.rx_clkm_div_yn1 = yn1;
    HW_REG_SET_FIELD(I2S0.rx_clkm_conf, rx_clkm_div_num, integer);
    I2S0.rx_clkm_div_conf.val = d.val;

    I2S0.rx_conf1.rx_bck_div_num = MIC_BCK_DIV - 1u;

    /* Actual rate: src / (integer + num/den) / 256 */
    uint64_t q = (uint64_t)integer * den + num;
    s_actual_rate = (uint32_t)(((uint64_t)MIC_SRC_CLK_HZ * den) / (q * MIC_MCLK_MULT));
}

static void mic_setup_format(uint32_t block_bytes)
{
    I2S0.rx_conf.rx_reset = 1;
    I2S0.rx_conf.rx_reset = 0;
    I2S0.rx_conf.rx_fifo_reset = 1;
    I2S0.rx_conf.rx_fifo_reset = 0;

    /* Philips standard: master, stereo, MSB first, data delayed one BCK after WS edge. */
    I2S0.rx_conf.rx_slave_mod = 0;
    I2S0.rx_conf.rx_mono = 0;
    I2S0.rx_conf.rx_mono_fst_vld = 0;
    I2S0.rx_conf.rx_big_endian = 0;
    I2S0.rx_conf.rx_pcm_bypass = 1;
    I2S0.rx_conf.rx_left_align = 1;
    I2S0.rx_conf.rx_24_fill_en = 0;
    I2S0.rx_conf.rx_ws_idle_pol = 0;        /* WS low = left slot */
    I2S0.rx_conf.rx_bit_order = 0;
    I2S0.rx_conf.rx_pdm_en = 0;
    I2S0.rx_conf.rx_tdm_en = 1;             /* "std" mode is TDM with 2 slots on this chip */

    I2S0.rx_conf1.rx_bits_mod = MIC_SLOT_BITS - 1u;
    I2S0.rx_conf1.rx_tdm_chan_bits = MIC_SLOT_BITS - 1u;
    I2S0.rx_conf1.rx_half_sample_bits = MIC_SLOT_BITS - 1u;
    I2S0.rx_conf1.rx_tdm_ws_width = MIC_SLOT_BITS - 1u;
    I2S0.rx_conf1.rx_msb_shift = 1;

    uint32_t tdm = I2S0.rx_tdm_ctrl.val;
    tdm &= 0xFFFF0000u;
    tdm |= 0x3u;                             /* slots 0 and 1 active */
    I2S0.rx_tdm_ctrl.val = tdm;
    I2S0.rx_tdm_ctrl.rx_tdm_tot_chan_num = 1;  /* 2 slots */

    I2S0.rx_eof_num.rx_eof_num = block_bytes;   /* bytes per EOF (one DMA block) */
}

static void mic_setup_dma(uint32_t block_bytes)
{
    for (uint32_t i = 0; i < s_cfg.num_blocks; i++) {
        s_desc[i].size = block_bytes;
        s_desc[i].length = 0;
        s_desc[i].suc_eof = 0;
        s_desc[i].err_eof = 0;
        s_desc[i].owner = 1;
        s_desc[i].buffer = s_buf[i];
        s_desc[i].next = &s_desc[(i + 1u) % s_cfg.num_blocks];
    }

    GDMA.channel[MIC_DMA_CH].in.in_conf0.in_rst = 1;
    GDMA.channel[MIC_DMA_CH].in.in_conf0.in_rst = 0;
    GDMA.channel[MIC_DMA_CH].in.in_conf0.indscr_burst_en = 1;
    GDMA.channel[MIC_DMA_CH].in.in_conf0.in_data_burst_en = 1;
    GDMA.channel[MIC_DMA_CH].in.in_conf0.mem_trans_en = 0;
    GDMA.channel[MIC_DMA_CH].in.in_conf1.in_check_owner = 0;  /* endless ring */
    GDMA.channel[MIC_DMA_CH].in.in_peri_sel.sel = SOC_GDMA_TRIG_PERIPH_I2S0;
    GDMA.channel[MIC_DMA_CH].in.in_pri.rx_pri = 5;            /* highest */
    GDMA.channel[MIC_DMA_CH].in.in_link.addr = ((uint32_t)&s_desc[0]) & 0xFFFFFu;
}

int mic_init(const mic_config_t *cfg, mic_block_cb_t cb, void *arg)
{
    if (!cfg || cfg->frames_per_block == 0 || cfg->frames_per_block * 2u > MIC_MAX_WORDS ||
        cfg->num_blocks < 2 || cfg->num_blocks > MIC_MAX_BLOCKS || cfg->sample_rate < 8000u ||
        cfg->sample_rate > 48000u) {
        return HW_ERR_ARG;
    }
    s_cfg = *cfg;
    s_cb = cb;
    s_cb_arg = arg;

    hw_periph_enable(HW_PERIPH_GDMA);
    hw_periph_enable(HW_PERIPH_I2S);
    GDMA.misc_conf.clk_en = 1;

    hw_gpio_route_out(cfg->pin_sck, I2SI_BCK_OUT_IDX);
    hw_gpio_route_out(cfg->pin_ws, I2SI_WS_OUT_IDX);
    hw_gpio_route_in(cfg->pin_sd, I2SI_SD_IN_IDX);

    mic_setup_clock(cfg->sample_rate);

    GDMA.intr[MIC_DMA_CH].ena.val &= ~0x06A7u;       /* RX events of this channel off */
    GDMA.intr[MIC_DMA_CH].clr.val = 0x06A7u;
    if (!s_initialised) {
        if (hw_intr_attach(ETS_DMA_CH0_INTR_SOURCE + MIC_DMA_CH, mic_dma_isr, NULL) != HW_OK) {
            return HW_ERR_STATE;
        }
    }
    s_initialised = true;
    return HW_OK;
}

int mic_start(void)
{
    if (!s_initialised) {
        return HW_ERR_STATE;
    }
    if (s_running) {
        return HW_OK;
    }
    uint32_t block_bytes = s_cfg.frames_per_block * 2u * sizeof(int32_t);
    memset(s_buf, 0, sizeof(s_buf));
    s_blocks_done = 0;

    mic_setup_format(block_bytes);
    mic_setup_dma(block_bytes);

    GDMA.intr[MIC_DMA_CH].clr.val = 0x06A7u;
    GDMA.intr[MIC_DMA_CH].ena.val |= GDMA_EVT_RX_SUC_EOF;
    GDMA.channel[MIC_DMA_CH].in.in_link.start = 1;

    I2S0.rx_conf.rx_update = 1;
    while (I2S0.rx_conf.rx_update) {
    }
    I2S0.rx_conf.rx_start = 1;
    s_running = true;
    return HW_OK;
}

void mic_stop(void)
{
    if (!s_running) {
        return;
    }
    I2S0.rx_conf.rx_start = 0;
    GDMA.channel[MIC_DMA_CH].in.in_link.stop = 1;
    GDMA.intr[MIC_DMA_CH].ena.val &= ~GDMA_EVT_RX_SUC_EOF;
    GDMA.intr[MIC_DMA_CH].clr.val = 0x06A7u;
    s_running = false;
}

bool mic_is_running(void)
{
    return s_running;
}

const int32_t *mic_block(uint32_t idx)
{
    return s_buf[idx % s_cfg.num_blocks];
}

uint32_t mic_actual_sample_rate(void)
{
    return s_actual_rate;
}

uint32_t mic_blocks_completed(void)
{
    return s_blocks_done;
}
