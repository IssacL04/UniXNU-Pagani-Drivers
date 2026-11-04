/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* GENI 串行引擎的 UART 寄存器层：FIFO 模式、轮询、硬件流控。寄存器事实来自主线 geni-se.h、qcom_geni_serial.c 和本机 hsuart 跟踪
 * （只取事实，不抄代码）；偏移、打包和初始化值又和 pagani UEFI 的 HSUartDxe.efi 逐项核对过，它是这颗 SoC 上唯一走 FIFO 模式的高通驱动，
 * 水线和 stale 取它的值。SE 的通用初始化顺序和 PaganiSpeaker SpeakerHw.c、GeniI2c.c 一致。
 *
 * 为什么用 FIFO 轮询：SE DMA 要经 SMMU（SID 0x423），FIFO 不碰 SMMU。发送和接收都按 4 字节一个 FIFO 字打包（主线和 HSUartDxe 都这样）：
 * 收到的半个字要等 stale 计时（220 个比特时间）才带着 RX_LAST 出来，115200 时一个孤立的 0xfc 最多晚 1.9 ms，都在 10 ms 唤醒窗口内。
 *
 * 轮询的代价：半个字出来后还没读走，又收满下一个字，RX_LAST 就说不清前面那半个字了，它按整字读出来，多 1 到 3 个补位字节（模型假定是 0，
 * 真机未知）。stale 要线路空闲才到，所以补位几乎只落在帧与帧之间，H4 解析器在帧边界当杂字节跳过（junk 计数），帧不丢；帧中途停顿超过
 * stale 才会真的坏一帧。主线走中断、HSUartDxe 只是调试口，都不处理这件事。波特率越高 stale 越短，23、28、33、38 档要上机看 junk。
 *
 * 全部走回调，内核接真窗口，主机测试接模型（Test/geni_model.h）。 */
#ifndef PAGANI_BT_GENI_H
#define PAGANI_BT_GENI_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PB_SE_FORCE_DEFAULT 0x20u
#define PB_SE_OUTPUT_CTRL 0x24u
#define PB_SE_CGC_CTRL 0x28u
#define PB_SE_STATUS 0x40u
#define PB_SE_M_CLK_CFG 0x48u
#define PB_SE_S_CLK_CFG 0x4cu
#define PB_SE_IF_DISABLE 0x64u
#define PB_SE_FW_REVISION 0x68u
#define PB_SE_CLK_SEL 0x7cu
#define PB_SE_UART_LOOPBACK 0x22cu
#define PB_SE_BYTE_GRAN 0x254u
#define PB_SE_DMA_MODE 0x258u
#define PB_SE_UART_TX_TRANS_CFG 0x25cu
#define PB_SE_TX_PACK0 0x260u
#define PB_SE_TX_PACK1 0x264u
#define PB_SE_UART_TX_WORD_LEN 0x268u
#define PB_SE_UART_TX_STOP_LEN 0x26cu
#define PB_SE_UART_TX_TRANS_LEN 0x270u
#define PB_SE_UART_RX_TRANS_CFG 0x280u
#define PB_SE_RX_PACK0 0x284u
#define PB_SE_RX_PACK1 0x288u
#define PB_SE_UART_RX_WORD_LEN 0x28cu
#define PB_SE_UART_RX_STALE 0x294u
#define PB_SE_UART_TX_PARITY 0x2a4u
#define PB_SE_UART_RX_PARITY 0x2a8u
#define PB_SE_UART_MANUAL_RFR 0x2acu
#define PB_SE_M_CMD0 0x600u
#define PB_SE_M_CMD_CTRL 0x604u
#define PB_SE_M_IRQ_STATUS 0x610u
#define PB_SE_M_IRQ_EN 0x614u
#define PB_SE_M_IRQ_CLEAR 0x618u
#define PB_SE_S_CMD0 0x630u
#define PB_SE_S_CMD_CTRL 0x634u
#define PB_SE_S_IRQ_STATUS 0x640u
#define PB_SE_S_IRQ_EN 0x644u
#define PB_SE_S_IRQ_CLEAR 0x648u
#define PB_SE_TX_FIFO 0x700u
#define PB_SE_RX_FIFO 0x780u
#define PB_SE_TX_FIFO_STATUS 0x800u
#define PB_SE_RX_FIFO_STATUS 0x804u
#define PB_SE_TX_WATERMARK 0x80cu
#define PB_SE_RX_WATERMARK 0x810u
#define PB_SE_RFR_WATERMARK 0x814u
#define PB_SE_IOS 0x908u
#define PB_SE_GSI_EVENT_EN 0xe18u
#define PB_SE_HW_PARAM_0 0xe24u
#define PB_SE_HW_PARAM_1 0xe28u
#define PB_SE_DMA_GENERAL_CFG 0xe30u

/* 字段 */
#define PB_PROTO_UART 2u                    /* GENI_FW_REVISION 的 15:8 */
#define PB_STATUS_M_ACTIVE (1u << 0)
#define PB_STATUS_S_ACTIVE (1u << 12)
#define PB_CMD_CTRL_ABORT (1u << 1)
#define PB_CMD_CTRL_CANCEL (1u << 2)
#define PB_IRQ_DONE (1u << 0)
#define PB_IRQ_OVERRUN (1u << 1)
#define PB_IRQ_ILLEGAL (1u << 2)
#define PB_IRQ_FAILURE (1u << 3)
#define PB_IRQ_CANCEL (1u << 4)
#define PB_IRQ_ABORT (1u << 5)
#define PB_IRQ_RX_RD_ERR (1u << 24)
#define PB_IRQ_RX_WR_ERR (1u << 25)
#define PB_IRQ_RX_WATERMARK (1u << 26)
#define PB_IRQ_RX_LAST (1u << 27)
#define PB_IRQ_TX_RD_ERR (1u << 28)
#define PB_IRQ_TX_WR_ERR (1u << 29)
#define PB_M_IRQ_ERRORS (PB_IRQ_OVERRUN | PB_IRQ_ILLEGAL | PB_IRQ_FAILURE | PB_IRQ_TX_RD_ERR | PB_IRQ_TX_WR_ERR)
#define PB_M_IRQ_WANTED (PB_IRQ_DONE | PB_IRQ_CANCEL | PB_IRQ_ABORT | PB_M_IRQ_ERRORS)
#define PB_S_IRQ_GP (0x1fu << 9)            /* 奇偶、断线 */
#define PB_S_IRQ_ERRORS (PB_IRQ_OVERRUN | PB_IRQ_ILLEGAL | PB_IRQ_FAILURE | PB_IRQ_RX_RD_ERR | PB_IRQ_RX_WR_ERR)
#define PB_S_IRQ_WANTED (PB_IRQ_CANCEL | PB_IRQ_ABORT | PB_S_IRQ_ERRORS | PB_S_IRQ_GP | PB_IRQ_RX_WATERMARK | PB_IRQ_RX_LAST)
#define PB_UART_START_TX (1u << 27)
#define PB_UART_START_RX (1u << 27)          /* 参数全 0：RFR_OPEN（bit 7）只属于 DMA 读，HSUartDxe 和主线的 FIFO 路径都不带 */
#define PB_UART_CTS_MASK (1u << 1)          /* TX_TRANS_CFG：置位时忽略 CTS（跟踪 "trans_cfg2"），清零时听 CTS（"trans_cfg0"） */
#define PB_UART_RFR_MANUAL (1u << 31)
#define PB_UART_RFR_NOT_READY (1u << 1)     /* 跟踪 "rfr = 0x80000002" */
#define PB_UART_LOOPBACK_ALL 3u             /* TX 接 RX，RFR 接 CTS */
#define PB_RX_FIFO_WC 0x1ffffffu
#define PB_RX_FIFO_LAST (1u << 31)
#define PB_RX_FIFO_LAST_SHIFT 28
#define PB_TX_FIFO_WC 0x0fffffffu
#define PB_IOS_RX (1u << 0)
#define PB_IOS_CTS (1u << 1)                /* 0：对端允许发送。跟踪里健康状态 geni_ios:0x1 */
#define PB_PACK0_4X8 0x4380eu               /* 8 位、每字 4 个、低字节先上线 */
#define PB_PACK1_4X8 0xc3e0eu
#define PB_BYTES_PER_WORD 4u
/* 下面四个是 HSUartDxe 的值；主线中断路径是 RX 2、RFR 深度减 2、stale 160。这里轮询 FIFO 状态，TX、RX 水线只影响 IRQ 状态位，
 * RFR 比主线多留两个字给对端刹车。 */
#define PB_TX_WATERMARK 4u
#define PB_RX_WATERMARK_GAP 8u              /* 深度减 8 */
#define PB_RFR_WATERMARK_GAP 4u             /* 深度减 4 时拉高 RFR */
#define PB_STALE_BITS 220u                  /* 线路空闲这么多比特时间，半个字才出来 */

enum {
    PbGeniOk = 0, PbGeniErrProto = -1, PbGeniErrNoFifo = -2, PbGeniErrTimeout = -3, PbGeniErrBus = -4, PbGeniErrArgument = -5,
    PbGeniErrBusy = -6, PbGeniErrCancel = -7,
};

typedef struct {
    void *ctx;
    uint32_t (*read)(void *ctx, uint32_t reg);
    void (*write)(void *ctx, uint32_t reg, uint32_t value);
    void (*delay_us)(void *ctx, unsigned us);   /* 短等，不让出 CPU */
    void (*sleep_ms)(void *ctx, unsigned ms);
    uint64_t (*now_ms)(void *ctx);
} PbGeniIo;

typedef struct {uint32_t fw, proto, version, if_disable, status, ios, param0, param1, m_clk, tx_depth, rx_depth;} PbGeniInfo;

typedef struct {
    PbGeniIo io;
    uint32_t tx_depth, rx_depth;            /* FIFO 深度，单位是字 */
    uint8_t rx_running, ready;
    uint32_t tx_bytes, rx_bytes, tx_timeouts, m_sticky, s_sticky;
    uint32_t rx_odd_tails;                  /* RX_FIFO_STATUS 最高位清零、字节数字段却是 1..3 的次数，见 pb_geni_read */
} PbGeni;

/* 只读：固件协议、FIFO 是否可用、FIFO 深度。协议不是 UART（PbGeniErrProto）或 FIFO 被关（PbGeniErrNoFifo）时不写任何寄存器。 */
int pb_geni_probe(PbGeni *g, const PbGeniIo *io, PbGeniInfo *info);
/* 配成 8N1、FIFO、4×8 打包。div 是 SE 分频，level 是 CLK_SEL（RCG 动态频率表的级，表没开时是 0）。收尾时 RFR 手动为“未就绪”，
 * CTS 忽略，接收未启动。 */
int pb_geni_config(PbGeni *g, unsigned div, unsigned level);
int pb_geni_set_clock(PbGeni *g, unsigned div, unsigned level);
/* cts：发送听 CTS。rfr_auto：RFR 交给硬件水线；0 时手动“未就绪”，叫对端别发。 */
void pb_geni_flow(PbGeni *g, int cts, int rfr_auto);
int pb_geni_rx_start(PbGeni *g);
int pb_geni_rx_stop(PbGeni *g);
void pb_geni_loopback(PbGeni *g, int on);
uint32_t pb_geni_ios(PbGeni *g);
/* 阻塞发送，直到 M_CMD_DONE 或 timeout_ms。超时会取消命令并返回 PbGeniErrTimeout；CTS 一直不放时就走这条路。 */
int pb_geni_write(PbGeni *g, const uint8_t *data, uint32_t bytes, unsigned timeout_ms);
/* 不阻塞：取走 FIFO 里已有的字节，最多 max（至少 4），返回字节数。 */
uint32_t pb_geni_read(PbGeni *g, uint8_t *out, uint32_t max);

#ifdef __cplusplus
}
#endif
#endif
