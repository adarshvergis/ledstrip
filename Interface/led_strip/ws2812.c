/**
 * @file ws2812.c
 * @brief WS2812B driver on ESP32-C3 GPSPI2 (master, MOSI only) fed by GDMA.
 *
 * SPI clock: PLL_F80M / 32 = 2.5 MHz. Frame = 1 lead-in zero byte, 9 bytes per LED,
 * then LATCH_BYTES zero bytes (>= 280 us low, as required by WS2812B-V5).
 */
#include "ws2812.h"

#include <string.h>
#include "build_config.h"
#include "hw_common.h"

#include "soc/spi_struct.h"
#include "soc/gdma_struct.h"
#include "soc/gdma_channel.h"
#include "soc/gpio_sig_map.h"
#include "esp_attr.h"
#include "esp_rom_sys.h"

#define LED_DMA_CH          HW_GDMA_CH_LED
#define SPI_DIV             32u                     /* 80 MHz / 32 = 2.5 MHz        */
#define SPI_BIT_NS          400u
#define LEAD_BYTES          1u
#define LATCH_BYTES         100u                    /* 100 * 8 * 0.4 us = 320 us    */
#define BYTES_PER_LED       9u                      /* 3 colours * 3 SPI bytes      */
#define TX_BUF_SIZE         (LEAD_BYTES + LED_MAX_COUNT * BYTES_PER_LED + LATCH_BYTES)
#define BUSY_TIMEOUT_US     5000u

_Static_assert(TX_BUF_SIZE <= 4095, "single DMA descriptor limit");

static DRAM_ATTR uint8_t       s_tx[2][(TX_BUF_SIZE + 3u) & ~3u] __attribute__((aligned(4)));
static DRAM_ATTR hw_dma_desc_t s_desc[2] __attribute__((aligned(4)));
static uint32_t s_lut[256];          /* colour byte -> 24 SPI bits */
static uint8_t  s_back;              /* buffer to encode into next */
static bool     s_started;
static bool     s_initialised;
static uint16_t s_max_leds;

/* Byte position of R, G, B on the wire for each colour order. */
static const uint8_t s_order_map[LED_ORDER_COUNT][3] = {
    /* r  g  b */
    {1, 0, 2},  /* GRB */
    {0, 1, 2},  /* RGB */
    {1, 2, 0},  /* BRG */
    {0, 2, 1},  /* RBG */
    {2, 0, 1},  /* GBR */
    {2, 1, 0},  /* BGR */
};

static void build_lut(void)
{
    for (uint32_t v = 0; v < 256; v++) {
        uint32_t out = 0;
        for (int bit = 7; bit >= 0; bit--) {
            out = (out << 3) | ((v & (1u << bit)) ? 0x6u : 0x4u);   /* 110 : 100 */
        }
        s_lut[v] = out;
    }
}

int ws2812_init(uint32_t gpio, uint16_t max_leds)
{
    if (max_leds == 0 || max_leds > LED_MAX_COUNT) {
        return HW_ERR_ARG;
    }
    s_max_leds = max_leds;
    build_lut();
    memset(s_tx, 0, sizeof(s_tx));

    hw_periph_enable(HW_PERIPH_GDMA);
    hw_periph_enable(HW_PERIPH_SPI2);
    GDMA.misc_conf.clk_en = 1;

    /* SPI master, MOSI only, mode 0, MSB first, MOSI idles low. */
    GPSPI2.clk_gate.clk_en = 1;
    GPSPI2.clk_gate.mst_clk_active = 1;
    GPSPI2.clk_gate.mst_clk_sel = 1;           /* PLL_F80M */
    GPSPI2.slave.val = 0;
    GPSPI2.user.val = 0;
    GPSPI2.user1.val = 0;
    GPSPI2.user2.val = 0;
    GPSPI2.ctrl.val = 0;                       /* d_pol = 0 -> MOSI low when idle */
    GPSPI2.misc.val = 0;
    GPSPI2.misc.cs0_dis = 1;
    GPSPI2.misc.cs1_dis = 1;
    GPSPI2.misc.cs2_dis = 1;
    GPSPI2.misc.cs3_dis = 1;
    GPSPI2.misc.cs4_dis = 1;
    GPSPI2.misc.cs5_dis = 1;
    GPSPI2.user.usr_mosi = 1;

    __typeof__(GPSPI2.clock) clk;
    clk.val = 0;
    clk.clkcnt_n = SPI_DIV - 1u;
    clk.clkcnt_h = SPI_DIV / 2u - 1u;
    clk.clkcnt_l = SPI_DIV - 1u;
    clk.clkdiv_pre = 0;
    GPSPI2.clock.val = clk.val;

    GPSPI2.dma_conf.val = 0;
    GPSPI2.dma_conf.tx_seg_trans_clr_en = 1;
    GPSPI2.dma_conf.rx_seg_trans_clr_en = 1;
    GPSPI2.dma_conf.dma_tx_ena = 1;
    GPSPI2.dma_int_ena.val = 0;
    GPSPI2.dma_int_clr.val = 0xFFFFFFFFu;

    /* GDMA TX channel -> SPI2 */
    GDMA.channel[LED_DMA_CH].out.out_conf0.out_rst = 1;
    GDMA.channel[LED_DMA_CH].out.out_conf0.out_rst = 0;
    GDMA.channel[LED_DMA_CH].out.out_conf0.outdscr_burst_en = 1;
    GDMA.channel[LED_DMA_CH].out.out_conf0.out_data_burst_en = 1;
    GDMA.channel[LED_DMA_CH].out.out_conf0.out_eof_mode = 1;
    GDMA.channel[LED_DMA_CH].out.out_conf1.out_check_owner = 0;
    GDMA.channel[LED_DMA_CH].out.out_peri_sel.sel = SOC_GDMA_TRIG_PERIPH_SPI2;
    GDMA.channel[LED_DMA_CH].out.out_pri.tx_pri = 3;

    hw_gpio_route_out(gpio, FSPID_OUT_IDX);

    GPSPI2.cmd.update = 1;
    while (GPSPI2.cmd.update) {
    }
    s_initialised = true;
    s_started = false;
    return HW_OK;
}

bool ws2812_busy(void)
{
    return s_started && !GPSPI2.dma_int_raw.trans_done;
}

uint32_t ws2812_frame_time_us(uint16_t n)
{
    return ((LEAD_BYTES + n * BYTES_PER_LED + LATCH_BYTES) * 8u * SPI_BIT_NS) / 1000u;
}

static void encode(uint8_t *dst, const led_rgb_t *px, uint16_t n, led_color_order_t order)
{
    const uint8_t *map = s_order_map[order < LED_ORDER_COUNT ? order : LED_ORDER_GRB];
    uint8_t *p = dst + LEAD_BYTES;
    for (uint16_t i = 0; i < n; i++) {
        uint8_t wire[3];
        wire[map[0]] = px[i].r;
        wire[map[1]] = px[i].g;
        wire[map[2]] = px[i].b;
        for (int c = 0; c < 3; c++) {
            uint32_t bits = s_lut[wire[c]];
            *p++ = (uint8_t)(bits >> 16);
            *p++ = (uint8_t)(bits >> 8);
            *p++ = (uint8_t)bits;
        }
    }
    memset(p, 0, LATCH_BYTES);
}

int ws2812_show(const led_rgb_t *px, uint16_t n, led_color_order_t order)
{
    if (!s_initialised || !px) {
        return HW_ERR_STATE;
    }
    if (n > s_max_leds) {
        n = s_max_leds;
    }
    uint8_t *buf = s_tx[s_back];
    encode(buf, px, n, order);
    uint32_t len = LEAD_BYTES + n * BYTES_PER_LED + LATCH_BYTES;

    int ret = HW_OK;
    uint32_t t0 = hw_cycles();
    while (ws2812_busy()) {
        if (hw_cycles_to_us(hw_cycles() - t0) > BUSY_TIMEOUT_US) {
            ret = HW_ERR_BUSY;          /* hung transfer: fall through and restart */
            break;
        }
    }

    hw_dma_desc_t *d = &s_desc[s_back];
    d->size = (len + 3u) & ~3u;
    d->length = len;
    d->suc_eof = 1;
    d->owner = 1;
    d->buffer = buf;
    d->next = NULL;

    GDMA.channel[LED_DMA_CH].out.out_conf0.out_rst = 1;
    GDMA.channel[LED_DMA_CH].out.out_conf0.out_rst = 0;
    GPSPI2.dma_conf.dma_afifo_rst = 1;
    GPSPI2.dma_conf.dma_afifo_rst = 0;
    GPSPI2.dma_conf.buf_afifo_rst = 1;
    GPSPI2.dma_conf.buf_afifo_rst = 0;
    GDMA.channel[LED_DMA_CH].out.out_link.addr = ((uint32_t)d) & 0xFFFFFu;
    GDMA.channel[LED_DMA_CH].out.out_link.start = 1;

    GPSPI2.ms_dlen.ms_data_bitlen = len * 8u - 1u;
    GPSPI2.dma_int_clr.trans_done = 1;
    GPSPI2.cmd.update = 1;
    while (GPSPI2.cmd.update) {
    }
    GPSPI2.cmd.usr = 1;
    s_started = true;
    s_back ^= 1u;
    return ret;
}
