/*
 * pong.c — firmware bare-metal MicroBlaze V, Pong en Nexys A7-100T
 *
 * Arquitectura de renderizado:
 *   MicroBlaze escribe píxeles directamente al framebuffer BRAM (Port A, 32-bit AXI).
 *   El VGA lee Port B en cada pixel clock y convierte el índice de paleta a RGB.
 *
 *   Framebuffer: 640×480 × 4-bit = 38 400 palabras de 32 bits (big-endian, 8 px/word).
 *   word[31:28] = píxel 0 (x mod 8 == 0) … word[3:0] = píxel 7 (x mod 8 == 7).
 *
 * Periféricos:
 *   XPAR_AXI_BRAM_CTRL_0_BASEADDR   — framebuffer VRAM (Port A)
 *   XPAR_AXI_GPIO_0_BASEADDR        — GPIO0 ch1: botones {BTNR,BTNL,BTNC,BTND,BTNU}
 *                                     GPIO0 ch2: SW[0] modo 2P
 *   XPAR_AXI_GPIO_1_BASEADDR        — GPIO1 ch1: LED[14:0]
 *   XPAR_AXI_QUAD_SPI_0_BASEADDR    — SPI inter-FPGA modo 2P
 *   XPAR_AXI_QUAD_SPI_1_BASEADDR    — SPI microSD (requiere rebuild de plataforma)
 */

#include "xparameters.h"
#include "xgpio.h"
#include "xspi.h"
#include "xil_io.h"
#include "sleep.h"

/* ── SD SPI: dirección provisional hasta rebuild de plataforma ─────────── */
#ifndef XPAR_AXI_QUAD_SPI_1_BASEADDR
#define XPAR_AXI_QUAD_SPI_1_BASEADDR  0x44A10000UL
#endif

/* ── Framebuffer ──────────────────────────────────────────────────────────── */
#if defined(XPAR_AXI_BRAM_CTRL_0_BASEADDR)
#  define FB_BASE  XPAR_AXI_BRAM_CTRL_0_BASEADDR
#elif defined(XPAR_AXI_BRAM_CTRL_0_S_AXI_BASEADDR)
#  define FB_BASE  XPAR_AXI_BRAM_CTRL_0_S_AXI_BASEADDR
#else
#  define FB_BASE  0xC0000000UL
#endif
#define FB_W     640
#define FB_H     480
#define FB_WORDS 38400   /* (640×480) / 8 */

/* ── DDR2 (MIG 7-series, calibración automática al arrancar) ────────────── */
#if defined(XPAR_MIG_0_BASEADDRESS)
#  define DDR2_BASE  XPAR_MIG_0_BASEADDRESS
#elif defined(XPAR_MIG7SERIES_0_BASEADDR)
#  define DDR2_BASE  XPAR_MIG7SERIES_0_BASEADDR
#else
#  define DDR2_BASE  0x80000000UL
#endif
#define DDR2_SPR_BALL   ((u8 *)(DDR2_BASE + 0x40000UL))  /* 32 B,  tras 256 KB de código */
#define DDR2_SPR_PADDLE ((u8 *)(DDR2_BASE + 0x40020UL))  /* 240 B */
#define DDR2_SPR_LOGO     ((u8 *)(DDR2_BASE + 0x40110UL))  /* 512 B */
#define DDR2_SPR_GAMEOVER ((u8 *)(DDR2_BASE + 0x41000UL))  /* 22500 B — 200×225 4bpp (3 secciones) */
#define DDR2_SPR_MSEL     ((u8 *)(DDR2_BASE + 0x47000UL))  /* 11200 B — 200×112 4bpp */
#define DDR2_SPR_PAUSE    ((u8 *)(DDR2_BASE + 0x4A000UL))  /* 8000 B  — 200×80  4bpp */
#define SPR_GO_W         200
#define SPR_GO_SEC_H      75   /* alto de cada sección (3 secciones × 75 = 225) */
#define SPR_GO_SEC_BYTES (SPR_GO_W * SPR_GO_SEC_H / 2)  /* 7500 B por sección */
#define SPR_PAUSE_W      200
#define SPR_PAUSE_H       80

/* ── Paleta (debe coincidir con top_pong_project.v) ──────────────────────── */
#define COL_BLACK   0
#define COL_WHITE   1
#define COL_RED     2
#define COL_BLUE    3
#define COL_YELLOW  4
#define COL_GREEN   5
#define COL_ORANGE  6
#define COL_GRAY    7
#define COL_DGRAY   8
#define COL_MAGENTA 9

/* ── Geometría del juego ──────────────────────────────────────────────────── */
#define BALL_TICK_DIV 1    /* pelota avanza cada frame (60 Hz, vsync real) */
#define BALL_SZ    8
#define PAD_W      8
#define PAD_H      60
#define PAD1_X     20
#define PAD2_X     612
#define PAD_SPEED  5
#define SCORE_WIN  10

/* ── Botones (GPIO0 canal 1: bit = {BTNR,BTNL,BTNC,BTND,BTNU}) ──────────── */
#define BTN_U  0x01u
#define BTN_D  0x02u
#define BTN_C  0x04u
#define BTN_L  0x08u
#define BTN_R  0x10u
#define BTN_VSYNC 0x20u  /* GPIO0 bit 5 = vsync (activo bajo) */

/* ── Estados del juego ───────────────────────────────────────────────────── */
#define ST_MENU     0
#define ST_PLAYING  1
#define ST_PAUSE    2
#define ST_GAMEOVER 3
#define ST_WAIT_2P  4   /* espera handshake SPI antes de iniciar 2P */

/* ── Split-screen 2P ─────────────────────────────────────────────────────── */
#define GAME_W   1280             /* ancho total del campo (2 pantallas) */
#define PAD2_X_G (FB_W + PAD2_X) /* game-x paleta derecha: 1252 */
#define SPI_PING 0xA5u
#define SPI_PONG 0x5Au

/* ── AXI Quad SPI registros (offset desde base, PG153 v3.2) ─────────────── */
#define SPI_SRR   0x40u   /* Software Reset Register (offset 0x40, no 0x00!) */
#define SPI_CR    0x60u   /* Control Register */
#define SPI_SR    0x64u   /* Status Register */
#define SPI_DTR   0x68u   /* TX FIFO */
#define SPI_DRR   0x6Cu   /* RX FIFO */
#define SPI_SSR   0x70u   /* Slave Select (activo bajo) */

#define SPICR_RXRST    (1u << 6)   /* Reset RX FIFO */
#define SPICR_TXRST    (1u << 5)   /* Reset TX FIFO */
#define SPICR_INHIBIT  (1u << 8)
#define SPICR_MANSS    (1u << 7)
#define SPICR_MASTER   (1u << 2)
#define SPICR_SPE      (1u << 1)
#define SPICR_LOOP     (1u << 0)   /* Loopback: MOSI: MISO internamente */
#define SPISR_RX_EMPTY (1u << 0)   /* 1 = RX FIFO vacío */
#define SPISR_TX_EMPTY (1u << 2)

/* ── Tipos ───────────────────────────────────────────────────────────────── */
typedef struct { int x, y, dx, dy; } ball_t;
typedef struct { int y; }             pad_t;

/* ── Sprites (4-bit packed, high nibble = px izquierdo) ──────────────────── */
#define SPR_BALL_W   8
#define SPR_BALL_H   8
#define SPR_PAD_W    8
#define SPR_PAD_H    60
#define SPR_LOGO_W   64
#define SPR_LOGO_H   16
#define SPR_MSEL_W   200
#define SPR_MSEL_H   112

/* Sprites viven en DDR2 — ver DDR2_SPR_BALL / _PADDLE / _LOGO */
static u8 sd_sector_buf[512];

/* Layout de sectores en la SD (LBA, antes de la partición: sector 0 = MBR) */
#define SD_MAGIC        0x504F4E47u   /* "PONG" */
#define SD_LBA_HDR      1
#define SD_LBA_BALL     2
#define SD_LBA_PADDLE   3
#define SD_LBA_LOGO     5   /* 512 B = 1 sector exacto */
#define SD_LBA_GAMEOVER 6   /* 200×225 4bpp = 22500 B = 44 sectores (LBA 6-49) */
#define SD_LBA_MSEL    50   /* 200×112 4bpp = 11200 B = 22 sectores (LBA 50-71) */
#define SD_LBA_PAUSE   72   /* 200×80  4bpp = 8000 B  = 16 sectores (LBA 72-87) */

/* ── Estado global ───────────────────────────────────────────────────────── */
static ball_t  ball;
static pad_t   pad[2];
static int     score[2];
static int     game_state;
static int     selected;
static int     mode_2p;
static int     is_slave  = 0;   /* 0=maestro (izquierda), 1=esclavo (derecha) */
static int     sd_ok      = 0;
static int     sd_sdhc       = 0;
static int     load_step     = 0;    /* 0=no ran, 1=hdr ok, 2=ball ok, 3=paddle ok, 4=logo ok */
static int     msel_loaded = 0;
static int     pause_loaded = 0;

/* Dirty-rect state — evita fb_clear() por frame */
static int     ball_tick         = 0;
static int     hit_count         = 0;
static int     bounce_count      = 0;
static u32     rng_state         = 73856093u;
static int     score_dirty       = 1;
static int     needs_full_redraw = 1;
static int     prev_game_state   = -1;
static int     prev_selected     = -1;
static int     prev_ball_x, prev_ball_y;
static int     prev_pad0_y, prev_pad1_y;

static XGpio   gpio0;
static XGpio   gpio1;
static XSpi    spi;

/* ═══════════════════════════════════════════════════════════════════════════
 * RENDERER — escritura al framebuffer BRAM
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Borra el framebuffer entero con un color uniforme (8 px/word, sin lectura) */
static void fb_clear(u8 color)
{
    u8  c = color & 0xF;
    u32 p = (u32)c * 0x11111111u;   /* replica el nibble 8 veces */
    for (u32 i = 0; i < FB_WORDS; i++)
        Xil_Out32(FB_BASE + (i << 2), p);
}

/* Espera flanco de subida del vsync. Si el bit no está disponible (GPIO <6 bits) regresa
   inmediatamente para no bloquear el loop. */
static void wait_vsync(void)
{
    int t;
    /* Si ya estamos DENTRO del pulso (vsync LOW): esperar a que termine.
     * Nota: el guard anterior hacía return inmediato aquí, lo que permitía
     * 2-3 iteraciones de game logic por frame visual: efecto de pintado. */
    if (!(Xil_In32(XPAR_AXI_GPIO_0_BASEADDR) & BTN_VSYNC)) {
        t = 200000;
        while (!(Xil_In32(XPAR_AXI_GPIO_0_BASEADDR) & BTN_VSYNC) && --t);
        return;  /* back porch comenzó — safe para renderizar */
    }
    /* Esperar LOW (inicio del pulso vsync) */
    t = 2000000;
    while ((Xil_In32(XPAR_AXI_GPIO_0_BASEADDR) & BTN_VSYNC) && --t);
    if (!t) return;
    /* Esperar HIGH (fin del pulso = inicio del back porch) */
    t = 200000;
    while (!(Xil_In32(XPAR_AXI_GPIO_0_BASEADDR) & BTN_VSYNC) && --t);
}

static u32 rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* Dibuja un rectángulo relleno.  Usa read-modify-write solo en nibbles parciales. */
static void fb_fill_rect(int x, int y, int w, int h, u8 color)
{
    u8  c     = color & 0xF;
    int x1    = (x < 0)    ? 0   : x;
    int x2    = (x+w > FB_W) ? FB_W : x+w;
    int y1    = (y < 0)    ? 0   : y;
    int y2    = (y+h > FB_H) ? FB_H : y+h;

    for (int row = y1; row < y2; row++) {
        int col = x1;

        /* Nibbles iniciales hasta alineación a 8 px */
        while (col < x2 && (col & 7)) {
            u32 pidx  = (u32)row * FB_W + col;
            u32 widx  = pidx >> 3;
            u32 nib   = pidx & 7u;
            u32 addr  = FB_BASE + (widx << 2);
            u32 shift = (7u - nib) * 4u;
            u32 word  = Xil_In32(addr);
            word = (word & ~(0xFu << shift)) | ((u32)c << shift);
            Xil_Out32(addr, word);
            col++;
        }

        /* Palabras completas (8 píxeles alineados) — solo escritura */
        u32 pattern = (u32)c * 0x11111111u;
        while (col + 8 <= x2) {
            u32 pidx = (u32)row * FB_W + col;
            Xil_Out32(FB_BASE + ((pidx >> 3) << 2), pattern);
            col += 8;
        }

        /* Nibbles finales */
        while (col < x2) {
            u32 pidx  = (u32)row * FB_W + col;
            u32 widx  = pidx >> 3;
            u32 nib   = pidx & 7u;
            u32 addr  = FB_BASE + (widx << 2);
            u32 shift = (7u - nib) * 4u;
            u32 word  = Xil_In32(addr);
            word = (word & ~(0xFu << shift)) | ((u32)c << shift);
            Xil_Out32(addr, word);
            col++;
        }
    }
}

/* Font 4×5 para dígitos 0-9.  bit 3 = columna izquierda, bit 0 = columna derecha. */
static const u8 font4x5[10][5] = {
    {0x6, 0x9, 0x9, 0x9, 0x6},  /* 0 */
    {0x2, 0x6, 0x2, 0x2, 0x7},  /* 1 */
    {0x6, 0x9, 0x2, 0x4, 0xF},  /* 2 */
    {0xE, 0x1, 0x6, 0x1, 0xE},  /* 3 */
    {0x9, 0x9, 0xF, 0x1, 0x1},  /* 4 */
    {0xF, 0x8, 0xE, 0x1, 0xE},  /* 5 */
    {0x6, 0x8, 0xE, 0x9, 0x6},  /* 6 */
    {0xF, 0x1, 0x2, 0x4, 0x4},  /* 7 */
    {0x6, 0x9, 0x6, 0x9, 0x6},  /* 8 */
    {0x6, 0x9, 0x7, 0x1, 0x6},  /* 9 */
};

/*
 * Dibuja un dígito en (x,y) con factor de escala `scale` (px por celda).
 * Resultado: (4×scale) × (5×scale) píxeles.
 */
static void fb_draw_digit(int x, int y, int digit, int scale, u8 color)
{
    const u8 *g = font4x5[digit % 10];
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 4; col++) {
            if (g[row] & (0x8u >> col))
                fb_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
        }
    }
}

/* Dibuja el marcador centrado arriba: dos dígitos a cada lado de la red.
 * x=256 y x=360 son múltiplos de 8 (alineados a word del FB de 4 bits/px).
 * Cada dígito ocupa 24 px (4 cols × 6 px) = exactamente 3 words: writes
 * directos sin RMW. También pinta el fondo negro, restaurando el área completa. */
static void fb_draw_scores(void)
{
    static const u32 BW[2] = {256/8, 360/8};   /* índice de word base */
    for (int d = 0; d < 2; d++) {
        const u8 *g = font4x5[score[d] % 10];
        u32 wb = BW[d];
        for (int fr = 0; fr < 5; fr++) {
            u32 c0 = (g[fr] & 0x8u) ? 1u : 0u;
            u32 c1 = (g[fr] & 0x4u) ? 1u : 0u;
            u32 c2 = (g[fr] & 0x2u) ? 1u : 0u;
            u32 c3 = (g[fr] & 0x1u) ? 1u : 0u;
            /* 8 nibbles por word, nibble 0 = MSB (bits 31:28).
             * Distribución por col (6 px/col): w0=c0×6+c1×2, w1=c1×4+c2×4, w2=c2×2+c3×6 */
            u32 w0=(c0<<28)|(c0<<24)|(c0<<20)|(c0<<16)|(c0<<12)|(c0<<8)|(c1<<4)|c1;
            u32 w1=(c1<<28)|(c1<<24)|(c1<<20)|(c1<<16)|(c2<<12)|(c2<<8)|(c2<<4)|c2;
            u32 w2=(c2<<28)|(c2<<24)|(c3<<20)|(c3<<16)|(c3<<12)|(c3<<8)|(c3<<4)|c3;
            for (int sr = 0; sr < 6; sr++) {
                u32 wi = (u32)(8 + fr*6 + sr) * (FB_W/8) + wb;
                Xil_Out32(FB_BASE + (wi     << 2), w0);
                Xil_Out32(FB_BASE + ((wi+1) << 2), w1);
                Xil_Out32(FB_BASE + ((wi+2) << 2), w2);
            }
        }
    }
}

/* Dibuja o borra exactamente BALL_SZ=8 píxeles de ancho.
 * Usa 1 write (alineado) o 2 RMW (no alineado) por fila — vs los 8 RMW
 * pixel a pixel de fb_fill_rect — para que cada word del framebuffer se
 * actualice atómicamente y la VGA no lea estados intermedios. */
static void fb_put_ball(int x, int y, u8 color)
{
    u8 c = color & 0xF;
    u32 c32 = (u32)c * 0x11111111u;
    int y1 = y < 0 ? 0 : y;
    int y2 = (y + BALL_SZ) > FB_H ? FB_H : (y + BALL_SZ);
    int wl = (x < 0 ? 0 : x) >> 3;
    int k  = (x < 0 ? 0 : x) & 7;
    if (k == 0) {
        for (int row = y1; row < y2; row++) {
            u32 wi = (u32)row * (FB_W / 8) + wl;
            Xil_Out32(FB_BASE + (wi << 2), c32);
        }
    } else {
        u32 ml = (1u << ((8 - k) * 4)) - 1u;  /* nibbles del word izquierdo */
        u32 mr = ~ml;                            /* nibbles del word derecho  */
        int has_right = (wl + 1 < (int)(FB_W / 8));
        for (int row = y1; row < y2; row++) {
            u32 wi = (u32)row * (FB_W / 8) + wl;
            u32 al = FB_BASE + (wi       << 2);
            Xil_Out32(al, (Xil_In32(al) & mr) | (c32 & ml));
            if (has_right) {
                u32 ar = FB_BASE + ((wi + 1) << 2);
                Xil_Out32(ar, (Xil_In32(ar) & ml) | (c32 & mr));
            }
        }
    }
}

/* Borra un strip rectangular a negro y restaura los elementos del fondo
 * que caen dentro de ese strip (línea central, paletas, dígitos). */
static void restore_bg_strip(int x, int y, int w, int h)
{
    fb_fill_rect(x, y, w, h, COL_BLACK);
    if (!mode_2p && x < 322 && x + w > 318) {
        int ry = y & ~7;
        for (; ry < y + h; ry += 8) {
            int y0 = ry     > y     ? ry     : y;
            int y1 = ry + 4 < y + h ? ry + 4 : y + h;
            if (y0 < y1) fb_fill_rect(318, y0, 4, y1 - y0, COL_GRAY);
        }
    }
    if ((!mode_2p || !is_slave) && x < PAD1_X + PAD_W && x + w > PAD1_X) {
        int iy1 = y > pad[0].y           ? y : pad[0].y;
        int iy2 = y + h < pad[0].y+PAD_H ? y + h : pad[0].y + PAD_H;
        if (iy1 < iy2) fb_fill_rect(PAD1_X, iy1, PAD_W, iy2-iy1, COL_WHITE);
    }
    if ((!mode_2p || is_slave) && x < PAD2_X + PAD_W && x + w > PAD2_X) {
        int iy1 = y > pad[1].y           ? y : pad[1].y;
        int iy2 = y + h < pad[1].y+PAD_H ? y + h : pad[1].y + PAD_H;
        if (iy1 < iy2) fb_fill_rect(PAD2_X, iy1, PAD_W, iy2-iy1, COL_WHITE);
    }
}

/*
 * Blit de sprite 4-bit (high nibble = px izquierdo) al framebuffer.
 * Si transparent != 0, los píxeles con valor 0 (negro) no se escriben.
 */
/* Blit con escala entera. Fast-path para sc=2 con x alineado a 8 píxeles FB:
 * empaqueta 4 píxeles del sprite en 1 word FB (32 bits) y lo escribe sin
 * leer BRAM cuando todos los píxeles del grupo son opacos. Reduce writes y
 * elimina casi todos los RMW de BRAM comparado con la ruta fill_rect. */
static void fb_blit_scaled(int x, int y, int w, int h, const u8 *spr, int transparent, int sc)
{
    /* ── Fast path: sc=2, x alineado a word FB (x%8==0) ── */
    if (sc == 2 && (x & 7) == 0) {
        for (int row = 0; row < h; row++) {
            u32 cword = 0; int cbase = -4;
            int col = 0;
            while (col + 4 <= w) {
                /* Leer 2 bytes del sprite (4 píxeles) desde DDR2 con caché */
                int bi0 = (row * w + col) >> 1;
                int wb0 = bi0 & ~3;
                if (wb0 != cbase) { cword = Xil_In32((u32)(uintptr_t)(spr + wb0)); cbase = wb0; }
                u8 b0 = (cword >> ((bi0 & 3) << 3)) & 0xFF;

                int bi1 = bi0 + 1, wb1 = bi1 & ~3;
                if (wb1 != cbase) { cword = Xil_In32((u32)(uintptr_t)(spr + wb1)); cbase = wb1; }
                u8 b1 = (cword >> ((bi1 & 3) << 3)) & 0xFF;

                u8 p0 = b0 >> 4, p1 = b0 & 0xF, p2 = b1 >> 4, p3 = b1 & 0xF;

                /* Empaquetar en word FB: cada px: 2 nibbles adyacentes */
                u32 pval = ((u32)p0<<28)|((u32)p0<<24)|((u32)p1<<20)|((u32)p1<<16)
                          |((u32)p2<<12)|((u32)p2<< 8)|((u32)p3<< 4)| (u32)p3;
                u32 mask = 0;
                if (transparent) {
                    if (p0) mask |= 0xFF000000u;
                    if (p1) mask |= 0x00FF0000u;
                    if (p2) mask |= 0x0000FF00u;
                    if (p3) mask |= 0x000000FFu;
                } else { mask = 0xFFFFFFFFu; }

                if (mask) {
                    u32 fa = FB_BASE + (((u32)(y + row*2)   * FB_W + x + col*2) >> 3) * 4;
                    u32 fb_ = FB_BASE + (((u32)(y + row*2+1) * FB_W + x + col*2) >> 3) * 4;
                    if (mask == 0xFFFFFFFFu) {
                        Xil_Out32(fa,  pval);
                        Xil_Out32(fb_, pval);
                    } else {
                        Xil_Out32(fa,  (Xil_In32(fa)  & ~mask) | (pval & mask));
                        Xil_Out32(fb_, (Xil_In32(fb_) & ~mask) | (pval & mask));
                    }
                }
                col += 4;
            }
            /* Píxeles restantes (< 4) con ruta genérica */
            while (col < w) {
                int idx = row * w + col;
                int bi = idx >> 1, wb = bi & ~3;
                if (wb != cbase) { cword = Xil_In32((u32)(uintptr_t)(spr + wb)); cbase = wb; }
                u8 cur = (cword >> ((bi & 3) << 3)) & 0xFF;
                u8 px  = (idx & 1) ? (cur & 0xF) : (cur >> 4);
                if (!transparent || px) fb_fill_rect(x + col*2, y + row*2, 2, 2, px);
                col++;
            }
        }
        return;
    }

    /* ── Ruta genérica: cualquier sc, cualquier x ── */
    for (int row = 0; row < h; row++) {
        int col = 0;
        u32 cword = 0;
        int cbase = -4;
        while (col < w) {
            int idx   = row * w + col;
            int bi    = idx >> 1;
            int wbase = bi & ~3;
            if (wbase != cbase) {
                cword = Xil_In32((u32)(uintptr_t)(spr + wbase));
                cbase = wbase;
            }
            u8 cur = (cword >> ((bi & 3) << 3)) & 0xFF;
            u8 px  = (idx & 1) ? (cur & 0xF) : (cur >> 4);
            if (transparent && px == 0) { col++; continue; }
            int run = 1;
            while (col + run < w) {
                int idx2   = idx + run;
                int bi2    = idx2 >> 1;
                int wbase2 = bi2 & ~3;
                if (wbase2 != cbase) {
                    cword = Xil_In32((u32)(uintptr_t)(spr + wbase2));
                    cbase = wbase2;
                }
                u8 b2  = (cword >> ((bi2 & 3) << 3)) & 0xFF;
                u8 px2 = (idx2 & 1) ? (b2 & 0xF) : (b2 >> 4);
                if (px2 != px) break;
                run++;
            }
            fb_fill_rect(x + col * sc, y + row * sc, run * sc, sc, px);
            col += run;
        }
    }
}

/* Mini-fuente de letras: A,D,E,G,I,L,N,R,S,T,U (4×5 px, nibble MSB = col izquierdo) */
static const u8 font_ext[16][5] = {
    {0x6, 0x9, 0xF, 0x9, 0x9},  /* A */
    {0xE, 0x9, 0x9, 0x9, 0xE},  /* D */
    {0xF, 0x8, 0xE, 0x8, 0xF},  /* E */
    {0x7, 0x8, 0xB, 0x9, 0x7},  /* G */
    {0xF, 0x6, 0x6, 0x6, 0xF},  /* I */
    {0x7, 0x1, 0x1, 0x9, 0x6},  /* J */
    {0x8, 0x8, 0x8, 0x8, 0xF},  /* L */
    {0x9, 0xF, 0xF, 0x9, 0x9},  /* M */
    {0x9, 0xD, 0xB, 0x9, 0x9},  /* N */
    {0x6, 0x9, 0x9, 0x9, 0x6},  /* O */
    {0xE, 0x9, 0xE, 0x8, 0x8},  /* P */
    {0xE, 0x9, 0xE, 0xA, 0x9},  /* R */
    {0x7, 0x8, 0x6, 0x1, 0xE},  /* S */
    {0xF, 0x6, 0x6, 0x6, 0x6},  /* T */
    {0x9, 0x9, 0x9, 0x9, 0x6},  /* U */
    {0x9, 0x9, 0x9, 0x6, 0x6},  /* V */
};

static int char_idx(char c) {
    switch (c) {
        case 'A': return 0; case 'D': return 1; case 'E': return 2;
        case 'G': return 3; case 'I': return 4; case 'J': return 5;
        case 'L': return 6; case 'M': return 7; case 'N': return 8;
        case 'O': return 9; case 'P': return 10; case 'R': return 11;
        case 'S': return 12; case 'T': return 13; case 'U': return 14;
        case 'V': return 15;
        default:  return -1;
    }
}

static void fb_draw_str(int x, int y, const char *s, int scale, u8 color) {
    while (*s) {
        char c = *s++;
        const u8 *g = NULL;
        if (c >= '0' && c <= '9')  g = font4x5[c - '0'];
        else { int idx = char_idx(c); if (idx >= 0) g = font_ext[idx]; }
        if (g) {
            for (int row = 0; row < 5; row++)
                for (int col = 0; col < 4; col++)
                    if (g[row] & (0x8u >> col))
                        fb_fill_rect(x + col*scale, y + row*scale, scale, scale, color);
        }
        x += 4*scale + 2;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SD CARD — driver mínimo SPI manual (modo SPI 0, CPOL=0 CPHA=0)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define SD_BASE   XPAR_AXI_QUAD_SPI_1_BASEADDR
#define SPI2P_BASE XPAR_AXI_QUAD_SPI_0_BASEADDR

static void sd_spi_setup(void)
{
    /* Software reset AXI Quad SPI (SRR offset correcto: 0x40) */
    Xil_Out32(SD_BASE + SPI_SRR, 0x0Au);
    usleep(100);
    /* Resetear TX y RX FIFOs via bits CR[5:6] */
    Xil_Out32(SD_BASE + SPI_CR,
              SPICR_INHIBIT | SPICR_MANSS | SPICR_MASTER | SPICR_SPE |
              SPICR_TXRST | SPICR_RXRST);
    usleep(10);
    /* Modo normal: master, manual SS, transfer inhibited, SPE */
    Xil_Out32(SD_BASE + SPI_CR, SPICR_INHIBIT | SPICR_MANSS | SPICR_MASTER | SPICR_SPE);
    Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);   /* CS desasertado */
}

static u8 sd_spi_byte(u8 tx)
{
    /* Drenar RX FIFO de datos obsoletos antes de iniciar nueva transferencia */
    for (int d = 0; d < 100000; d++) {
        if (Xil_In32(SD_BASE + SPI_SR) & SPISR_RX_EMPTY) break;
        (void)Xil_In32(SD_BASE + SPI_DRR);
    }

    u32 cr = Xil_In32(SD_BASE + SPI_CR);
    Xil_Out32(SD_BASE + SPI_DTR, tx);
    Xil_Out32(SD_BASE + SPI_CR, cr & ~SPICR_INHIBIT);  /* iniciar transferencia */

    /* Esperar TX vacío Y RX con dato: garantiza que el shift register terminó */
    for (int t = 0; t < 100000; t++) {
        u32 sr = Xil_In32(SD_BASE + SPI_SR);
        if ((sr & SPISR_TX_EMPTY) && !(sr & SPISR_RX_EMPTY)) break;
    }
    Xil_Out32(SD_BASE + SPI_CR, cr);  /* detener transferencia (INHIBIT) */
    /* IP v3.2 retorna SLVERR si se lee DRR con FIFO vacío — evitar trap */
    if (Xil_In32(SD_BASE + SPI_SR) & SPISR_RX_EMPTY) return 0xFF;
    return (u8)Xil_In32(SD_BASE + SPI_DRR);
}

/* Verifica que el IP AXI Quad SPI funcione enviando 0x5A en loopback.
 * Retorna 1 si el byte enviado = byte recibido, 0 si hay error. */
static int sd_loopback_test(void)
{
    u32 cr = Xil_In32(SD_BASE + SPI_CR);
    Xil_Out32(SD_BASE + SPI_CR, cr | SPICR_LOOP);   /* activar loopback */
    u8 echo = sd_spi_byte(0x5A);
    Xil_Out32(SD_BASE + SPI_CR, cr);                  /* desactivar loopback */
    return (echo == 0x5A) ? 1 : 0;
}

/*
 * Inicializa la SD en modo SPI.
 * Retorna  0: OK
 *         -1: CMD0 sin respuesta (tarjeta no detectada o SCK no llega)
 *         -2: ACMD41 timeout (tarjeta no sale de idle — posible SD dañada)
 */
static int sd_init(void)
{
    sd_spi_setup();

    /* 80 pulsos de clock con CS desasertado */
    Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);
    for (int i = 0; i < 10; i++) sd_spi_byte(0xFF);

    /* Aserta CS[0] — CMD0 en SPI mode requiere CS bajo ANTES del primer bit */
    Xil_Out32(SD_BASE + SPI_SSR, 0xFEu);

    /* CMD0: GO_IDLE_STATE — espera R1=0x01
     * Reintentar hasta 10 veces (con gap inter-cmd) en caso de mala sincronización. */
    const u8 cmd0[] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x95};
    u8 r1 = 0xFF;
    for (int attempt = 0; attempt < 20 && r1 != 0x01; attempt++) {
        for (int i = 0; i < 6; i++) sd_spi_byte(cmd0[i]);
        r1 = 0xFF;
        for (int i = 0; i < 10 && r1 == 0xFF; i++) r1 = sd_spi_byte(0xFF);
        if (r1 != 0x01) {
            /* Gap inter-comando: desaserta CS, 8 clocks, reaserta */
            Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);
            for (int i = 0; i < 1; i++) sd_spi_byte(0xFF);
            Xil_Out32(SD_BASE + SPI_SSR, 0xFEu);
            usleep(10000);
        }
    }
    if (r1 != 0x01) { Xil_Out32(SD_BASE + SPI_SSR, 0xFFu); return -1; }

    /* Gap post-CMD0: desaserta CS + 8 clocks + reaserta */
    Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);
    for (int i = 0; i < 1; i++) sd_spi_byte(0xFF);
    Xil_Out32(SD_BASE + SPI_SSR, 0xFEu);

    /* CMD8: SEND_IF_COND (SDHC check)
     * R1=0x01: SDHC (consume 4 echo bytes R7)
     * R1=0x05: SDSC legacy (no echo)
     * R1=0xFF: no responde (tarjeta antigua o sin soporte) */
    const u8 cmd8[] = {0x48, 0x00, 0x00, 0x01, 0xAA, 0x87};
    for (int i = 0; i < 6; i++) sd_spi_byte(cmd8[i]);
    u8 r8 = 0xFF;
    for (int i = 0; i < 8 && r8 == 0xFF; i++) r8 = sd_spi_byte(0xFF);
    if (r8 == 0x01) {
        for (int i = 0; i < 4; i++) sd_spi_byte(0xFF);  /* consume 4 echo bytes R7 */
    } else {
        for (int i = 0; i < 8; i++) sd_spi_byte(0xFF);  /* limpiar bus */
    }

    /* Gap post-CMD8 */
    Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);
    for (int i = 0; i < 1; i++) sd_spi_byte(0xFF);
    Xil_Out32(SD_BASE + SPI_SSR, 0xFEu);

    /* ACMD41: espera hasta salir de idle.
     * CS se toglea entre comandos: algunas tarjetas SDHC lo requieren.
     * Máx 200 intentos × 10 ms = 2 s de timeout. */
    int tries = 0;
    do {
        /* CMD55 — CS bajo, enviar, leer R1, CS alto + 8 clocks */
        Xil_Out32(SD_BASE + SPI_SSR, 0xFEu);
        const u8 cmd55[] = {0x77, 0x00, 0x00, 0x00, 0x00, 0x01};
        for (int i = 0; i < 6; i++) sd_spi_byte(cmd55[i]);
        r1 = 0xFF;
        for (int i = 0; i < 10 && r1 == 0xFF; i++) r1 = sd_spi_byte(0xFF);
        Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);
        sd_spi_byte(0xFF);   /* 8 clocks con CS alto */

        /* CMD41 — CS bajo, enviar, leer R1, CS alto + 8 clocks */
        Xil_Out32(SD_BASE + SPI_SSR, 0xFEu);
        const u8 cmd41[] = {0x69, 0x40, 0x00, 0x00, 0x00, 0x01};
        for (int i = 0; i < 6; i++) sd_spi_byte(cmd41[i]);
        r1 = 0xFF;
        for (int i = 0; i < 10 && r1 == 0xFF; i++) r1 = sd_spi_byte(0xFF);
        Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);
        sd_spi_byte(0xFF);   /* 8 clocks con CS alto */

        tries++;
        usleep(10000);   /* 10 ms por intento: 2 s de timeout total */
    } while (r1 != 0x00 && tries < 200);

    if (r1 != 0x00) { Xil_Out32(SD_BASE + SPI_SSR, 0xFFu); return -2; }

    /* Reaserta CS para CMD58 */
    Xil_Out32(SD_BASE + SPI_SSR, 0xFEu);

    /* CMD58: READ_OCR — detectar SDHC (bit CCS en OCR[30]) */
    const u8 cmd58[] = {0x7A, 0x00, 0x00, 0x00, 0x00, 0x01};
    for (int i = 0; i < 6; i++) sd_spi_byte(cmd58[i]);
    u8 r58 = 0xFF;
    for (int i = 0; i < 20 && r58 == 0xFF; i++) r58 = sd_spi_byte(0xFF);
    u8 ocr0 = sd_spi_byte(0xFF);
    sd_spi_byte(0xFF); sd_spi_byte(0xFF); sd_spi_byte(0xFF);  /* ocr[1..3] */
    sd_sdhc = (r58 == 0x00 && (ocr0 & 0x40)) ? 1 : 0;

    Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);
    return 0;
}

/*
 * Lee un bloque de 512 bytes desde el sector LBA.
 * SDHC: dirección por bloques.  SDSC: dirección por bytes (lba << 9).
 */
static int sd_read_block(u32 lba, u8 *buf)
{
    u32 addr = sd_sdhc ? lba : (lba << 9);

    /* Gap: 8 ciclos idle con CS desasertado */
    Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);
    for (int i = 0; i < 8; i++) sd_spi_byte(0xFF);

    Xil_Out32(SD_BASE + SPI_SSR, 0xFEu);   /* CS assert */

    u8 cmd[6] = { 0x51,
        (u8)(addr >> 24), (u8)(addr >> 16), (u8)(addr >> 8), (u8)addr, 0x01 };
    for (int i = 0; i < 6; i++) sd_spi_byte(cmd[i]);

    u8 r1 = 0xFF;
    for (int i = 0; i < 20 && r1 == 0xFF; i++) r1 = sd_spi_byte(0xFF);
    if (r1 != 0x00) { Xil_Out32(SD_BASE + SPI_SSR, 0xFFu); return -1; }

    /* N_AC_max = 100 ms @ 3.125 MHz = ~31250 bytes; usar 40000 con margen */
    u8 tok = 0xFF;
    for (int i = 0; i < 40000 && tok != 0xFE; i++) tok = sd_spi_byte(0xFF);
    if (tok != 0xFE) { Xil_Out32(SD_BASE + SPI_SSR, 0xFFu); return -1; }

    for (int i = 0; i < 512; i++) buf[i] = sd_spi_byte(0xFF);
    sd_spi_byte(0xFF); sd_spi_byte(0xFF);  /* CRC */

    Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);   /* CS deassert */
    return 0;
}

/*
 * Test de diagnóstico de la SD — imprime resultados por UART.
 * Retorna 1 si la SD quedó lista, 0 si falló (el juego sigue igual).
 *
 * Secuencia:
 *   1. sd_init()  — negociación SPI + detección SDHC
 *   2. Leer sector 0 (MBR) — verifica que SCK/MOSI/MISO funcionen
 *   3. Comprobar firma MBR 0x55AA en bytes 510-511
 */
static int sd_run_test(void)
{
    sd_spi_setup();
    if (!sd_loopback_test()) return 0;
    return (sd_init() == 0) ? 1 : 0;
}

static int sd_read_block_r(u32 lba, u8 *buf)
{
    for (int r = 0; r < 3; r++)
        if (sd_read_block(lba, buf) == 0) return 0;
    return -1;
}

/* Carga los sprites desde los sectores reservados de la SD. */
static void load_sprites(void)
{
    if (!sd_ok) return;

    /* Verificar magic "PONG" en sector cabecera */
    if (sd_read_block(SD_LBA_HDR, sd_sector_buf) != 0) return;
    u32 magic = ((u32)sd_sector_buf[0] << 24) | ((u32)sd_sector_buf[1] << 16) |
                ((u32)sd_sector_buf[2] <<  8) |  (u32)sd_sector_buf[3];
    if (magic != SD_MAGIC) return;
    load_step = 1;

    u8 *dst;

    if (sd_read_block(SD_LBA_BALL, sd_sector_buf) != 0) return;
    dst = DDR2_SPR_BALL;
    for (int i = 0; i < SPR_BALL_W * SPR_BALL_H / 2; i++) dst[i] = sd_sector_buf[i];
    load_step = 2;

    if (sd_read_block(SD_LBA_PADDLE, sd_sector_buf) != 0) return;
    dst = DDR2_SPR_PADDLE;
    for (int i = 0; i < SPR_PAD_W * SPR_PAD_H / 2; i++) dst[i] = sd_sector_buf[i];
    load_step = 3;

    if (sd_read_block(SD_LBA_LOGO, sd_sector_buf) != 0) return;
    dst = DDR2_SPR_LOGO;
    for (int i = 0; i < SPR_LOGO_W * SPR_LOGO_H / 2; i++) dst[i] = sd_sector_buf[i];
    load_step = 4;

    dst = DDR2_SPR_GAMEOVER;
    for (int s = 0; s < 44; s++) {
        if (sd_read_block_r(SD_LBA_GAMEOVER + s, sd_sector_buf) != 0) return;
        int loaded = s * 512;
        int remain = (SPR_GO_W * 225 / 2) - loaded;
        int copy   = (remain >= 512) ? 512 : remain;
        if (copy <= 0) break;
        for (int i = 0; i < copy; i++) dst[loaded + i] = sd_sector_buf[i];
    }
    load_step = 5;

    /* mode_select: 160×90 4bpp = 7200 B = 15 sectores */
    dst = DDR2_SPR_MSEL;
    for (int s = 0; s < 22; s++) {
        if (sd_read_block_r(SD_LBA_MSEL + s, sd_sector_buf) != 0) return;
        int loaded = s * 512;
        int remain = (SPR_MSEL_W * SPR_MSEL_H / 2) - loaded;
        int copy   = (remain >= 512) ? 512 : remain;
        if (copy <= 0) break;
        for (int i = 0; i < copy; i++) dst[loaded + i] = sd_sector_buf[i];
    }
    msel_loaded = 1;

    /* pause_menu: 200×80 4bpp = 8000 B = 16 sectores */
    dst = DDR2_SPR_PAUSE;
    for (int s = 0; s < 16; s++) {
        if (sd_read_block_r(SD_LBA_PAUSE + s, sd_sector_buf) != 0) return;
        int loaded = s * 512;
        int remain = (SPR_PAUSE_W * SPR_PAUSE_H / 2) - loaded;
        int copy   = (remain >= 512) ? 512 : remain;
        if (copy <= 0) break;
        for (int i = 0; i < copy; i++) dst[loaded + i] = sd_sector_buf[i];
    }
    pause_loaded = 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * LÓGICA DEL JUEGO
 * ═══════════════════════════════════════════════════════════════════════════ */

static void init_game(void)
{
    u32 r = rng_next();
    /* Posición Y aleatoria en la mitad central de la pantalla */
    ball.x  = mode_2p ? GAME_W / 2 : FB_W / 2;
    ball.y  = FB_H / 4 + (int)(r % (FB_H / 2));
    /* Dirección X: alterna hacia el último perdedor (r bit 0); dy: 2–4 aleatorio */
    ball.dx = (r & 1u) ? 3 : -3;
    ball.dy = (int)(1 + (rng_next() % 3));  /* 1, 2 o 3 */
    if (rng_next() & 1u) ball.dy = -ball.dy;
    pad[0].y = (FB_H - PAD_H) / 2;
    pad[1].y = (FB_H - PAD_H) / 2;
    ball_tick    = 0;
    hit_count    = 0;
    bounce_count = 0;
    needs_full_redraw = 1;
}

/* Detección barrida: verdadero si la pelota tocó o cruzó la paleta este frame */
static int collide(ball_t *b, int px, int py)
{
    /* Rango X que la pelota recorrió este frame (incluyendo posición previa) */
    int x0 = b->x - b->dx;  /* posición antes del movimiento */
    int xmin = x0 < b->x ? x0 : b->x;
    int xmax = x0 > b->x ? x0 + BALL_SZ : b->x + BALL_SZ;
    if (xmax <= px || xmin >= px + PAD_W) return 0;
    if (b->y + BALL_SZ <= py || b->y >= py + PAD_H) return 0;
    return 1;
}

static int check_score(void)
{
    for (int i = 0; i < 2; i++) {
        if (score[i] >= SCORE_WIN) {
            selected = i;
            score[0] = score[1] = 0;
            return i + 1;
        }
    }
    return 0;
}

static void move_local_pad(int p, int up)
{
    pad[p].y += up ? -PAD_SPEED : PAD_SPEED;
    if (pad[p].y < 0)           pad[p].y = 0;
    if (pad[p].y > FB_H - PAD_H) pad[p].y = FB_H - PAD_H;
}

static void move_ai(void)
{
    static int react_delay = 0;   /* frames que la IA tarda en reaccionar */
    static int hit_offset  = PAD_H / 2;
    static int aim_err     = 0;   /* error de puntería en px (persiste por rally) */
    static int last_dx_ai  = 0;

    /* Contar rebotes para anti-loop */
    if ((ball.dx > 0 && last_dx_ai < 0) || (ball.dx < 0 && last_dx_ai > 0))
        bounce_count++;

    /* Cuando la pelota empieza a venir hacia la IA: sortear reacción y puntería */
    if (ball.dx < 0 && last_dx_ai >= 0) {
        hit_offset = (int)(rng_next() % (PAD_H - BALL_SZ + 1));

        /* 27 % de probabilidad de reacción tardía (33-55 frames perdidos).
         * Con spd=3 y dx=7, la cobertura baja de ~273 px a ~108 px mínimo —
         * la IA no puede alcanzar esquinas si sale con retraso. */
        if ((int)(rng_next() % 100) < 27) {
            react_delay = 33 + (int)(rng_next() % 23);
            aim_err = (int)(rng_next() % 13) - 6;  /* ±6 px cuando llega tarde */
        } else {
            aim_err = (int)(rng_next() % 9) - 4;   /* ±4 px de imprecisión normal */
        }
    }
    last_dx_ai = ball.dx;

    /* Anti-loop: tras 8 rebotes sin gol, forzar zona opuesta del paddle */
    if (bounce_count >= 8) {
        bounce_count = 0;
        int mid = (PAD_H - BALL_SZ) / 2;
        if (hit_offset <= mid)
            hit_offset = mid + 2 + (int)(rng_next() % (mid - 2));
        else
            hit_offset = 1 + (int)(rng_next() % (mid - 2));
    }

    /* Retraso de reacción activo: la IA no se mueve aún */
    if (react_delay > 0) { react_delay--; return; }

    /* Seguimiento: 3 px/frame.
     * A dx=7 la pelota tarda ~91 frames en cruzar el campo; sin retraso
     * la IA cubre ~273 px — suficiente para alcanzar cualquier punto si
     * parte razonablemente cerca. Con 30 frames de retraso solo cubre ~183 px,
     * dejando las esquinas vulnerables si la IA estaba lejos. */
    int target = ball.y + BALL_SZ / 2 - hit_offset + aim_err;

    /* Snap al target cuando la distancia es menor que spd para evitar oscilación */
    if (ball.dx < 0) {
        int diff = target - pad[0].y;
        if      (diff >  3) pad[0].y += 3;
        else if (diff < -3) pad[0].y -= 3;
        else                pad[0].y += diff;
    } else {
        int mid = (FB_H - PAD_H) / 2;
        int diff = mid - pad[0].y;
        if      (diff >  3) pad[0].y += 3;
        else if (diff < -3) pad[0].y -= 3;
        else                pad[0].y += diff;
    }

    if (pad[0].y < 0)            pad[0].y = 0;
    if (pad[0].y > FB_H - PAD_H) pad[0].y = FB_H - PAD_H;
}

static void move_ball(void)
{
    int rpad_x     = mode_2p ? PAD2_X_G : PAD2_X;
    int right_wall = mode_2p ? GAME_W   : FB_W;

    ball.x += ball.dx;
    ball.y += ball.dy;

    if (ball.y < 0)               { ball.y = 0;              ball.dy = -ball.dy; }
    if (ball.y > FB_H - BALL_SZ)  { ball.y = FB_H - BALL_SZ; ball.dy = -ball.dy; }

    if (ball.x < 0)                    { score[1]++; score_dirty = 1; init_game(); return; }
    if (ball.x > right_wall - BALL_SZ) { score[0]++; score_dirty = 1; init_game(); return; }

    /* Colisión paleta izquierda */
    if (collide(&ball, PAD1_X, pad[0].y)) {
        int hit = (pad[0].y + PAD_H) - ball.y;
        int spd = 3 + (++hit_count);
        if (spd > 9) spd = 9;
        ball.dx = spd;
        ball.dy = (hit < 8) ? 2 : (hit < 16) ? 2 : (hit < 24) ? 1 :
                  (hit < 30) ? 1 : (hit < 32) ? 0 : (hit < 38) ? -1 :
                  (hit < 46) ? -1 : (hit < 54) ? -2 : -2;
        if (ball.x < PAD1_X + PAD_W + 2) ball.x = PAD1_X + PAD_W + 2;
    }

    /* Colisión paleta derecha */
    {
        int r_hit = collide(&ball, rpad_x, pad[1].y);
        if (r_hit) {
            int hit = (pad[1].y + PAD_H) - ball.y;
            int spd = 2 + (++hit_count);
            if (spd > 7) spd = 7;
            ball.dx = -spd;
            ball.dy = (hit < 8) ? 2 : (hit < 16) ? 2 : (hit < 24) ? 1 :
                      (hit < 30) ? 1 : (hit < 32) ? 0 : (hit < 38) ? -1 :
                      (hit < 46) ? -1 : (hit < 54) ? -2 : -2;
            if (ball.x > rpad_x - BALL_SZ - 2) ball.x = rpad_x - BALL_SZ - 2;
        }
    }
}

/* ── SPI inter-FPGA 2P ───────────────────────────────────────────────────── */
static u32 btn_prev = 0;

static u32 btn_edge(void)
{
    u32 cur  = XGpio_DiscreteRead(&gpio0, 1) & 0x1Fu;
    u32 edge = cur & ~btn_prev;
    btn_prev = cur;
    /* Suprimir BTN_C si BTN_U o BTN_D están activos en el mismo frame */
    if (cur & (BTN_U | BTN_D)) edge &= ~BTN_C;
    return edge;
}

static int sw_on(void)
{
    return (int)(XGpio_DiscreteRead(&gpio0, 2) & 0x1u);
}

/* Maestro: esclavo: estado completo del juego (8 bytes).
   Recibe: pad[1].y del esclavo. */
static void spi_exchange(void)
{
    u8 tx[8], rx[8];
    tx[0] = (u8)(ball.x    >> 8); tx[1] = (u8)ball.x;
    tx[2] = (u8)(ball.y    >> 8); tx[3] = (u8)ball.y;
    tx[4] = (u8)(pad[0].y  >> 8); tx[5] = (u8)pad[0].y;
    tx[6] = (u8)((game_state << 4) | (score[0] & 0xF));
    tx[7] = (u8)((selected   << 4) | (score[1] & 0xF));
    {
        int rc = XSpi_Transfer(&spi, tx, rx, 8);
        if (rc == XST_SUCCESS) {
            /* rx[0] = pad[1].y>>1 (solo byte 0 fiable en modo esclavo AXI SPI) */
            int remote_y = (int)rx[0] << 1;
            if (remote_y >= 0 && remote_y <= FB_H - PAD_H)
                pad[1].y = remote_y;
        }
    }
}

static void update_leds(void)
{
    u32 v = ((u32)(score[1] & 0xF) << 4) | (u32)(score[0] & 0xF);
    XGpio_DiscreteWrite(&gpio1, 1, v);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * RENDER_FRAME — dibuja el estado actual al framebuffer
 * ═══════════════════════════════════════════════════════════════════════════ */
static void render_frame(void)
{
    /* Detectar cambio de estado */
    if (game_state != prev_game_state) {
        int was_game = (prev_game_state == ST_PLAYING || prev_game_state == ST_PAUSE);
        int is_game  = (game_state     == ST_PLAYING || game_state     == ST_PAUSE);
        if (!was_game || !is_game || prev_game_state < 0) {
            needs_full_redraw = 1;
            score_dirty       = 1;
        }
        /* Al reanudar desde pausa: limpiar overlay */
        if (prev_game_state == ST_PAUSE && game_state == ST_PLAYING)
            needs_full_redraw = 1;
        /* Al entrar en pausa: forzar redibujado del overlay */
        if (game_state == ST_PAUSE)
            prev_selected = -1;
        prev_game_state = game_state;
    }

    switch (game_state) {

    /* ── MENÚ ─────────────────────────────────────────────────────────── */
    case ST_MENU:
        if (needs_full_redraw) {
            fb_clear(COL_BLACK);
            if (load_step >= 4)
                /* 64×13 px × scale 5 = 320×65 px, centrado en 640 */
                fb_blit_scaled(160, 30, SPR_LOGO_W, 13, DDR2_SPR_LOGO, 1, 5);
            else
                fb_draw_str(221, 30, "PONG", 12, COL_WHITE);
            needs_full_redraw = 0;
            prev_selected = -1;
        }
        if (prev_selected != selected) {
            if (msel_loaded) {
                /* Sprite mode_select 200×112 × scale2 = 400×224, x=120 y=128 */
                /* Highlight 1P: display y=167, h=38. Highlight 2P: display y=233, h=38 */
                static const int hl_y[2] = {167, 233};
                if (prev_selected < 0) {
                    /* Primera entrada al menú: blit completo (pantalla ya limpia) */
                    if (selected==0) fb_fill_rect(120, 167, 400, 38, COL_BLUE);
                    else             fb_fill_rect(120, 233, 400, 38, COL_BLUE);
                    fb_blit_scaled(120, 128, SPR_MSEL_W, SPR_MSEL_H, DDR2_SPR_MSEL, 1, 2);
                } else {
                    /* Cambio de botón: reblit solo las 2 franjas afectadas (~3× más rápido).
                     * 38 display px / scale 2 = 19 sprite rows por franja. */
                    const int spr_rows = 19;
                    fb_fill_rect(120, hl_y[prev_selected], 400, 38, COL_BLACK);
                    fb_blit_scaled(120, hl_y[prev_selected], SPR_MSEL_W, spr_rows,
                                   DDR2_SPR_MSEL + ((hl_y[prev_selected]-128)/2) * (SPR_MSEL_W/2),
                                   1, 2);
                    fb_fill_rect(120, hl_y[selected], 400, 38, COL_BLUE);
                    fb_blit_scaled(120, hl_y[selected], SPR_MSEL_W, spr_rows,
                                   DDR2_SPR_MSEL + ((hl_y[selected]-128)/2) * (SPR_MSEL_W/2),
                                   1, 2);
                }
            } else {
                fb_fill_rect(220, 175, 200, 45, (selected == 0) ? COL_YELLOW : COL_DGRAY);
                fb_draw_digit(312, 183, 1, 6, COL_BLACK);
                fb_fill_rect(220, 260, 200, 45, (selected == 1) ? COL_YELLOW : COL_DGRAY);
                fb_draw_digit(312, 268, 2, 6, COL_BLACK);
            }
            prev_selected = selected;
        }
        break;

    /* ── JUGANDO + PAUSA ─────────────────────────────────────────────── */
    case ST_PLAYING:
    case ST_PAUSE:
        if (needs_full_redraw) {
            fb_clear(COL_BLACK);
            if (!mode_2p)
                for (int ny = 0; ny < FB_H; ny += 8)
                    fb_fill_rect(318, ny, 4, 4, COL_GRAY);
            fb_draw_scores();
            score_dirty = 0;
            {
                int bsx  = is_slave ? ball.x - FB_W : ball.x;
                int bvis = mode_2p ? (is_slave ? (ball.x >= FB_W) : (ball.x < FB_W)) : 1;
                if (bvis) fb_fill_rect(bsx, ball.y, BALL_SZ, BALL_SZ, COL_WHITE);
            }
            if (!mode_2p || !is_slave)
                fb_fill_rect(PAD1_X, pad[0].y, PAD_W, PAD_H, COL_WHITE);
            if (!mode_2p || is_slave)
                fb_fill_rect(PAD2_X, pad[1].y, PAD_W, PAD_H, COL_WHITE);
            prev_ball_x = ball.x; prev_ball_y = ball.y;
            prev_pad0_y = pad[0].y; prev_pad1_y = pad[1].y;
            needs_full_redraw = 0;
            if (game_state == ST_PAUSE) {
                if (pause_loaded) {
                    /* pause sprite 200×80 ×2 = 400×160, centrado: x=120 y=160 */
                    fb_fill_rect(120, 160, 400, 160, COL_BLACK);
                    if (selected==0) fb_fill_rect(120, 175, 400, 49, COL_BLUE);
                    else             fb_fill_rect(120, 255, 400, 49, COL_BLUE);
                    fb_blit_scaled(120, 160, SPR_PAUSE_W, SPR_PAUSE_H, DDR2_SPR_PAUSE, 1, 2);
                } else {
                    fb_fill_rect(220, 175, 200, 45, (selected==0) ? COL_YELLOW : COL_DGRAY);
                    fb_draw_str(249, 187, "REANUDAR", 4, COL_BLACK);
                    fb_fill_rect(220, 260, 200, 45, (selected==1) ? COL_YELLOW : COL_DGRAY);
                    fb_draw_str(276, 272, "SALIR", 4, COL_BLACK);
                }
                prev_selected = selected;
            }
            break;
        }

        /* ── Rendering incremental: smart erase — nunca toca la intersección ── */
        {
            int old_bsx, old_vis, new_bsx, new_vis;
            if (!mode_2p) {
                old_bsx = prev_ball_x; old_vis = 1;
                new_bsx = ball.x;      new_vis = 1;
            } else if (!is_slave) {
                old_bsx = prev_ball_x; old_vis = (prev_ball_x < FB_W);
                new_bsx = ball.x;      new_vis = (ball.x < FB_W);
            } else {
                old_bsx = prev_ball_x - FB_W; old_vis = (prev_ball_x >= FB_W);
                new_bsx = ball.x - FB_W;      new_vis = (ball.x >= FB_W);
            }

            /* Paso 1: nueva pelota PRIMERO — 1-2 RMW/fila (atómico por word) */
            if (new_vis)
                fb_put_ball(new_bsx, ball.y, COL_WHITE);

            /* Paso 2: borrar SOLO los strips de la posición anterior que no
             * solapan con la nueva — la intersección nunca se toca (nunca negro) */
            if (old_vis) {
                int ob = prev_ball_y, oe = ob + BALL_SZ;
                int ol = old_bsx,     ore = ol + BALL_SZ;

                if (new_vis) {
                    int iy1 = ball.y + BALL_SZ > ob && ball.y < oe
                              ? (ball.y > ob ? ball.y : ob) : oe;
                    int iy2 = ball.y + BALL_SZ < oe
                              ? ball.y + BALL_SZ : oe;
                    int ix1 = new_bsx + BALL_SZ > ol && new_bsx < ore
                              ? (new_bsx > ol ? new_bsx : ol) : ore;
                    int ix2 = new_bsx + BALL_SZ < ore
                              ? new_bsx + BALL_SZ : ore;
                    int have_ov = (iy1 < iy2) && (ix1 < ix2);

                    if (have_ov) {
                        if (ob < iy1)  restore_bg_strip(ol, ob,  BALL_SZ, iy1-ob);
                        if (iy2 < oe)  restore_bg_strip(ol, iy2, BALL_SZ, oe-iy2);
                        if (ol < ix1)  restore_bg_strip(ol, iy1, ix1-ol,  iy2-iy1);
                        if (ix2 < ore) restore_bg_strip(ix2,iy1, ore-ix2, iy2-iy1);
                    } else {
                        restore_bg_strip(ol, ob, BALL_SZ, BALL_SZ);
                    }
                } else {
                    restore_bg_strip(ol, ob, BALL_SZ, BALL_SZ);
                }

                /* Restaurar score si la posición anterior lo tapaba; si la nueva
                 * posición también solapa el score, redibujar la pelota encima. */
                if (ob < 38 && oe > 8 &&
                    ((ol < 280 && ore > 256) || (ol < 384 && ore > 360))) {
                    fb_draw_scores();
                    if (new_vis && ball.y < 38 && ball.y + BALL_SZ > 8 &&
                        ((new_bsx < 280 && new_bsx + BALL_SZ > 256) ||
                         (new_bsx < 384 && new_bsx + BALL_SZ > 360)))
                        fb_put_ball(new_bsx, ball.y, COL_WHITE);
                }
            }

            prev_ball_x = ball.x; prev_ball_y = ball.y;
        }

        if (score_dirty) { fb_draw_scores(); score_dirty = 0; }

        /* Delta-rect paddles: solo la paleta propia de este FPGA */
        if ((!mode_2p || !is_slave) && pad[0].y != prev_pad0_y) {
            int dy = pad[0].y - prev_pad0_y;
            if (dy < 0 && -dy < PAD_H) {
                fb_fill_rect(PAD1_X, pad[0].y,         PAD_W, -dy, COL_WHITE);
                fb_fill_rect(PAD1_X, pad[0].y + PAD_H, PAD_W, -dy, COL_BLACK);
            } else if (dy > 0 && dy < PAD_H) {
                fb_fill_rect(PAD1_X, prev_pad0_y,         PAD_W, dy, COL_BLACK);
                fb_fill_rect(PAD1_X, prev_pad0_y + PAD_H, PAD_W, dy, COL_WHITE);
            } else {
                fb_fill_rect(PAD1_X, prev_pad0_y, PAD_W, PAD_H, COL_BLACK);
                fb_fill_rect(PAD1_X, pad[0].y,    PAD_W, PAD_H, COL_WHITE);
            }
            prev_pad0_y = pad[0].y;
        }
        if ((!mode_2p || is_slave) && pad[1].y != prev_pad1_y) {
            int dy = pad[1].y - prev_pad1_y;
            if (dy < 0 && -dy < PAD_H) {
                fb_fill_rect(PAD2_X, pad[1].y,         PAD_W, -dy, COL_WHITE);
                fb_fill_rect(PAD2_X, pad[1].y + PAD_H, PAD_W, -dy, COL_BLACK);
            } else if (dy > 0 && dy < PAD_H) {
                fb_fill_rect(PAD2_X, prev_pad1_y,         PAD_W, dy, COL_BLACK);
                fb_fill_rect(PAD2_X, prev_pad1_y + PAD_H, PAD_W, dy, COL_WHITE);
            } else {
                fb_fill_rect(PAD2_X, prev_pad1_y, PAD_W, PAD_H, COL_BLACK);
                fb_fill_rect(PAD2_X, pad[1].y,    PAD_W, PAD_H, COL_WHITE);
            }
            prev_pad1_y = pad[1].y;
        }

        /* Overlay de pausa: maestro y esclavo sincronizan selected vía SPI */
        if (game_state == ST_PAUSE && prev_selected != selected) {
            if (pause_loaded) {
                fb_fill_rect(120, 160, 400, 160, COL_BLACK);
                if (selected==0) fb_fill_rect(120, 175, 400, 49, COL_BLUE);
                else             fb_fill_rect(120, 255, 400, 49, COL_BLUE);
                fb_blit_scaled(120, 160, SPR_PAUSE_W, SPR_PAUSE_H, DDR2_SPR_PAUSE, 1, 2);
            } else {
                fb_fill_rect(220, 175, 200, 45, (selected==0) ? COL_YELLOW : COL_DGRAY);
                fb_draw_str(249, 187, "REANUDAR", 4, COL_BLACK);
                fb_fill_rect(220, 260, 200, 45, (selected==1) ? COL_YELLOW : COL_DGRAY);
                fb_draw_str(276, 272, "SALIR", 4, COL_BLACK);
            }
            prev_selected = selected;
        }
        break;

    /* ── ESPERA HANDSHAKE 2P (maestro) ─────────────────────────────── */
    case ST_WAIT_2P:
        if (needs_full_redraw) {
            fb_clear(COL_BLACK);
            fb_draw_str(115, 190, "MODO 2 JUGADORES", 5, COL_WHITE);
            fb_draw_str(110, 270, "ESPERANDO JUGADOR 2...", 3, COL_GRAY);
            needs_full_redraw = 0;
        }
        break;

    /* ── GAMEOVER ────────────────────────────────────────────────────── */
    case ST_GAMEOVER:
        if (needs_full_redraw) {
            fb_clear(COL_BLACK);
            {
                /* TOP=P1Wins, MID=P2Wins, BOT=ComputerWins
                   selected: 0=izq gana, 1=der gana */
                int sec;
                if (!mode_2p)
                    sec = (selected == 1) ? 0 : 2; /* 1P: humano=P1Wins, IA=ComputerWins */
                else
                    sec = (selected == 1) ? 0 : 1; /* 2P: maestro=P1Wins, esclavo=P2Wins */
                const u8 *spr = DDR2_SPR_GAMEOVER + sec * SPR_GO_SEC_BYTES;
                /* 200×75 ×3 = 600×225, centrado: x=20 y=90 */
                fb_blit_scaled(20, 90, SPR_GO_W, SPR_GO_SEC_H, spr, 0, 3);
            }
            needs_full_redraw = 0;
        }
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DDR2 — init y sprites por defecto
 * ═══════════════════════════════════════════════════════════════════════════ */

static void ddr2_init(void)
{
    /* MIG 7-series calibra solo al arrancar; calib_done sale en LED[15].
     * Sin señal accesible al CPU, se espera el peor caso documentado. */
    usleep(350000);
}

/*
 * Escribe 4 patrones en DDR2 y los lee de vuelta.
 * Muestra en LED[3:0] qué palabras pasaron (bit N = palabra N OK).
 * LED[15] = calib_done (siempre encendido si MIG calibró).
 * Resultado visible 2 segundos antes de que arranque el juego.
 */
static void ddr2_selftest(void)
{
    static const u32 pat[4] = {
        0xDEADBEEFu, 0x12345678u, 0xA5A5A5A5u, 0x0F0F0F0Fu
    };
    u32 base = DDR2_BASE + 0x20000UL;   /* 128 KB dentro de DDR2, lejos del código */
    u32 result = 0;

    for (int i = 0; i < 4; i++)
        Xil_Out32(base + (u32)(i * 4), pat[i]);

    for (int i = 0; i < 4; i++)
        if (Xil_In32(base + (u32)(i * 4)) == pat[i])
            result |= (1u << i);

    /* Mostrar en LED[3:0]: cada bit = palabra i correcta */
    XGpio_DiscreteWrite(&gpio1, 1, result);
    usleep(200000);    /* 200 ms para que el usuario vea los LEDs */
    XGpio_DiscreteWrite(&gpio1, 1, 0);
}

static void ddr2_sprite_defaults(void)
{
    /* Ball y paddle: blanco sólido (nibble COL_WHITE=1: byte 0x11).
     * Logo: negro/transparente hasta que la SD esté lista. */
    u32 addr;
    int i;

    addr = (u32)(uintptr_t)DDR2_SPR_BALL;
    for (i = 0; i < SPR_BALL_W * SPR_BALL_H / 8; i++)
        Xil_Out32(addr + (u32)(i * 4), 0x11111111u);

    addr = (u32)(uintptr_t)DDR2_SPR_PADDLE;
    for (i = 0; i < SPR_PAD_W * SPR_PAD_H / 8; i++)
        Xil_Out32(addr + (u32)(i * 4), 0x11111111u);

    addr = (u32)(uintptr_t)DDR2_SPR_LOGO;
    for (i = 0; i < SPR_LOGO_W * SPR_LOGO_H / 8; i++)
        Xil_Out32(addr + (u32)(i * 4), 0x00000000u);

}

/* ═══════════════════════════════════════════════════════════════════════════
 * SLAVE_LOOP — loop dedicado del esclavo (pantalla derecha, 2P).
 * Reconfigura SPI como esclavo, hace handshake y entra en bucle de render.
 * Nunca regresa.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void slave_loop(void)
{
    /* Reconfigurar SPI como esclavo */
    XSpi_Stop(&spi);
    XSpi_SetOptions(&spi, XSP_MANUAL_SSELECT_OPTION);
    XSpi_Start(&spi);
    XSpi_IntrGlobalDisable(&spi);

    /* Pantalla de espera */
    fb_clear(COL_BLACK);
    fb_draw_str(200, 190, "JUGADOR 2", 6, COL_WHITE);
    fb_draw_str(110, 270, "ESPERANDO JUGADOR 1...", 3, COL_GRAY);

    /* Handshake no bloqueante: polling manual FIFO con salida BTN_C */
    {
        /* Esperar que BTN_C se suelte (venía presionado del menú) */
        while (XGpio_DiscreteRead(&gpio0, 1) & BTN_C) wait_vsync();

        /* Forzar modo esclavo desde el inicio: MANSS|SPE, sin MASTER ni INHIBIT.
           Leer SPISR limpia MODF si quedó de la configuración de maestro. */
        (void)Xil_In32(SPI2P_BASE + SPI_SR);
        u32 cr = SPICR_MANSS | SPICR_SPE;
        Xil_Out32(SPI2P_BASE + SPI_CR, cr | SPICR_TXRST | SPICR_RXRST);
        Xil_Out32(SPI2P_BASE + SPI_CR, cr);

        u32 prev_lvl = 0;
        while (1) {
            u32 sr = Xil_In32(SPI2P_BASE + SPI_SR);

            /* Pre-cargar TX con 8× SPI_PONG si el FIFO está vacío.
               El maestro usa XSpi_Transfer(8 bytes); TX_EMPTY en AXI SPI se activa
               cuando el byte sale del FIFO al shift register, no cuando termina
               el clocking. Con 8 bytes hay tiempo para que RX llegue correctamente. */
            if (sr & SPISR_TX_EMPTY)
                for (int _i = 0; _i < 8; _i++)
                    Xil_Out32(SPI2P_BASE + SPI_DTR, (u32)SPI_PONG);

            wait_vsync();

            /* BTN_C flanco ascendente: salir al menú */
            u32 cur_lvl = XGpio_DiscreteRead(&gpio0, 1) & 0x1Fu;
            if ((cur_lvl & BTN_C) && !(prev_lvl & BTN_C)) {
                Xil_Out32(SPI2P_BASE + SPI_CR, cr | SPICR_INHIBIT);
                goto slave_exit;
            }
            prev_lvl = cur_lvl;

            /* ¿El maestro mandó un byte? */
            if (!(Xil_In32(SPI2P_BASE + SPI_SR) & SPISR_RX_EMPTY)) {
                u8 rx = (u8)Xil_In32(SPI2P_BASE + SPI_DRR);
                if (rx == SPI_PING) break;
            }
        }
        Xil_Out32(SPI2P_BASE + SPI_CR, cr | SPICR_INHIBIT);
    }

    /* Game loop: forzar CR a estado conocido esclavo, limpiar MODF leyendo SPISR */
    {
        (void)Xil_In32(SPI2P_BASE + SPI_SR);
        u32 c = SPICR_MANSS | SPICR_SPE;
        Xil_Out32(SPI2P_BASE + SPI_CR, c | SPICR_TXRST | SPICR_RXRST);
        Xil_Out32(SPI2P_BASE + SPI_CR, c);
        /* Precargar 8 ceros: si el master transfiere antes del primer RX drain,
           recibe 0x00 × 8 (sin slv_btnc ni coordenadas espurias). */
        for (int i = 0; i < 8; i++) Xil_Out32(SPI2P_BASE + SPI_DTR, 0u);
    }

    mode_2p           = 1;
    is_slave          = 1;
    game_state        = ST_PLAYING;
    needs_full_redraw = 1;
    score_dirty       = 1;
    init_game();

    {
        u32 gl_prev     = 0;
        u8  slv_btnc     = 0;
        int slv_selected = 0;
        int menu_consec  = 0;  /* paquetes ST_MENU consecutivos recibidos */
        int spi_watchdog = 0;  /* frames sin datos válidos del master: salida de emergencia */

        /* Sin preload inicial: FIFO vacío: steady-state desde el primer exchange.
           Si el master lee antes del primer reload, obtiene 0x00 × 8 (sin btnc). */

        while (1) {
        u32 lvl = XGpio_DiscreteRead(&gpio0, 1) & 0x1Fu;

        /* BTN_C flanco ascendente — solo señalizar al maestro; nunca salir localmente
           en ST_GAMEOVER (el maestro manda ST_MENU cuando corresponde salir) */
        if ((lvl & BTN_C) && !(gl_prev & BTN_C) && !(lvl & (BTN_U | BTN_D))) {
            if (game_state == ST_PLAYING || game_state == ST_PAUSE)
                slv_btnc = 1;
        }
        gl_prev = lvl;

        if (game_state == ST_MENU) goto slave_exit;

        if (game_state == ST_PAUSE) {
            if (lvl & BTN_U) slv_selected = 0;
            if (lvl & BTN_D) slv_selected = 1;
        } else {
            if (lvl & BTN_U) move_local_pad(1, 1);
            if (lvl & BTN_D) move_local_pad(1, 0);
        }

        update_leds();

        /* TX reload incondicional ANTES del drain: el FIFO siempre tiene datos
           frescos independientemente del vsync del maestro. 7 bytes (= original):
           byte 7 viene del shift register holdover (=0), evita overflow si el
           esclavo corre levemente más rápido que el maestro. */
        Xil_Out32(SPI2P_BASE + SPI_DTR, (u32)(pad[1].y >> 1));
        Xil_Out32(SPI2P_BASE + SPI_DTR, (u32)((slv_selected << 1) | slv_btnc));
        Xil_Out32(SPI2P_BASE + SPI_DTR, 0u);
        Xil_Out32(SPI2P_BASE + SPI_DTR, 0u);
        Xil_Out32(SPI2P_BASE + SPI_DTR, 0u);
        Xil_Out32(SPI2P_BASE + SPI_DTR, 0u);
        Xil_Out32(SPI2P_BASE + SPI_DTR, 0u);

        /* Drenar RX. */
        {
            u8 tmp[16];
            int n = 0;
            while (n < 16 && !(Xil_In32(SPI2P_BASE + SPI_SR) & SPISR_RX_EMPTY))
                tmp[n++] = (u8)Xil_In32(SPI2P_BASE + SPI_DRR);
            if (n >= 8) {
                spi_watchdog = 0;
                u8 *rx = tmp + (n - 8);
                {
                    int bx = ((int)rx[0] << 8) | rx[1];
                    int by = ((int)rx[2] << 8) | rx[3];
                    int p0 = ((int)rx[4] << 8) | rx[5];
                    if (bx >= 0 && bx < GAME_W && by >= 0 && by < FB_H) {
                        ball.x = bx; ball.y = by;
                    }
                    if (p0 >= 0 && p0 <= FB_H - PAD_H) pad[0].y = p0;
                }
                int ns   = (rx[6] >> 4) & 0xF;
                int ns0  = rx[6] & 0xF;
                int ns1  = rx[7] & 0xF;
                if (game_state != ST_PAUSE)
                    selected = (rx[7] >> 4) & 0xF;
                if (ns0 != score[0] || ns1 != score[1]) {
                    score_dirty       = 1;
                    needs_full_redraw = 1;
                }
                score[0] = ns0;
                score[1] = ns1;
                if (ns == ST_PAUSE && game_state != ST_PAUSE)
                    slv_selected = 0;
                if (ns >= ST_PLAYING && ns <= ST_GAMEOVER && ns != game_state) {
                    game_state        = ns;
                    needs_full_redraw = 1;
                }
                if (ns == ST_MENU) {
                    if (++menu_consec >= 2) goto slave_exit;
                } else {
                    menu_consec = 0;
                }
                slv_btnc = 0;  /* limpiar después de haber cargado en FIFO */
            } else {
                if (++spi_watchdog >= 120) goto slave_exit;
            }
        }

        if (game_state == ST_PAUSE) selected = slv_selected;
        wait_vsync();
        render_frame();
        } /* while(1) */
    }

slave_exit:
    /* Restaurar SPI como maestro y volver al menú */
    XSpi_Stop(&spi);
    XSpi_SetOptions(&spi, XSP_MASTER_OPTION | XSP_MANUAL_SSELECT_OPTION);
    XSpi_SetSlaveSelect(&spi, 0x01u);
    XSpi_Start(&spi);
    XSpi_IntrGlobalDisable(&spi);
    mode_2p          = 0;
    is_slave         = 0;
    game_state       = ST_MENU;
    needs_full_redraw = 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════════════ */
int main(void)
{
    /* Borrar framebuffer inmediatamente: BRAM accesible sin init periféricos.
     * Evita que la pantalla anterior permanezca visible durante el arranque. */
    fb_clear(COL_BLACK);

    /* GPIO0: botones (ch1 in) + SW (ch2 in) */
    XGpio_Initialize(&gpio0, XPAR_AXI_GPIO_0_BASEADDR);
    XGpio_SetDataDirection(&gpio0, 1, 0x3Fu);  /* 6 bits entrada: [5]=vsync [4:0]=botones */
    XGpio_SetDataDirection(&gpio0, 2, 0x01u);

    /* GPIO1: LEDs (ch1 out) */
    XGpio_Initialize(&gpio1, XPAR_AXI_GPIO_1_BASEADDR);
    XGpio_SetDataDirection(&gpio1, 1, 0x0000u);

    /* SPI0: inter-FPGA (maestro, polling) */
    XSpi_Config *spi_cfg = XSpi_LookupConfig(XPAR_AXI_QUAD_SPI_0_BASEADDR);
    XSpi_CfgInitialize(&spi, spi_cfg, spi_cfg->BaseAddress);
    XSpi_SetOptions(&spi, XSP_MASTER_OPTION | XSP_MANUAL_SSELECT_OPTION);
    XSpi_SetSlaveSelect(&spi, 0x01u);
    XSpi_Start(&spi);
    XSpi_IntrGlobalDisable(&spi);

    /* DDR2: esperar calibración MIG, verificar R/W y cargar sprites por defecto */
    ddr2_init();
    ddr2_selftest();
    ddr2_sprite_defaults();

    /* Estado inicial */
    game_state = ST_MENU;
    selected   = 0;
    mode_2p    = 0;
    score[0]   = score[1] = 0;
    init_game();
    usleep(500000);   /* 500 ms power-up SD antes de init SPI */
    sd_ok = sd_run_test();
    load_sprites();

    /* Frame inicial del menú */
    render_frame();

    int spi_2p_exit = 0;  /* frames de ST_MENU enviados al slave antes de limpiar mode_2p */
    while (1) {
        u32 btn = btn_edge();

        switch (game_state) {

        /* ── MENÚ PRINCIPAL ─────────────────────────────────────────────── */
        case ST_MENU:
            if (btn & BTN_U) selected = 0;
            if (btn & BTN_D) selected = 1;
            if (btn & BTN_C) {
                mode_2p  = (selected == 1) ? 1 : 0;
                score[0] = score[1] = 0;
                init_game();
                if (mode_2p) {
                    is_slave = sw_on();   /* SW0=0: maestro, SW0=1: esclavo */
                    if (is_slave) {
                        slave_loop();     /* regresa si BTN_C */
                    }
                    if (mode_2p) {        /* si slave salió, mode_2p=0: skip */
                        game_state        = ST_WAIT_2P;
                        needs_full_redraw = 1;
                    }
                } else {
                    game_state = ST_PLAYING;
                }
            }
            break;

        /* ── ESPERA 2P (maestro) ─────────────────────────────────────── */
        case ST_WAIT_2P: {
            if (btn & BTN_C) {
                mode_2p = 0; is_slave = 0;
                game_state = ST_MENU; needs_full_redraw = 1; break;
            }
            u8 tx[8] = {SPI_PING,SPI_PING,SPI_PING,SPI_PING,SPI_PING,SPI_PING,SPI_PING,SPI_PING};
            u8 rx[8] = {0};
            int rc = XSpi_Transfer(&spi, tx, rx, 8);
            int got_pong = 0;
            if (rc == XST_SUCCESS)
                for (int _i = 0; _i < 8; _i++) if (rx[_i] == SPI_PONG) { got_pong = 1; break; }
            if (got_pong) {
                score[0] = score[1] = 0;
                init_game();
                game_state        = ST_PLAYING;
                needs_full_redraw = 1;
            }
            break;
        }

        /* ── JUGANDO ────────────────────────────────────────────────────── */
        case ST_PLAYING:
            {
                u32 lvl = XGpio_DiscreteRead(&gpio0, 1) & 0x1Fu;
                /* Ignorar BTN_C si se presiona junto con movimiento (evita activación accidental) */
                if ((btn & BTN_C) && !(lvl & (BTN_U | BTN_D))) {
                    selected = 0; game_state = ST_PAUSE; break;
                }
                if (mode_2p) {
                    if (lvl & BTN_U) move_local_pad(0, 1);  /* maestro: paleta izquierda */
                    if (lvl & BTN_D) move_local_pad(0, 0);
                } else {
                    if (lvl & BTN_U) move_local_pad(1, 1);  /* 1P: paleta derecha */
                    if (lvl & BTN_D) move_local_pad(1, 0);
                }
            }

            if (!mode_2p) move_ai();

            if (++ball_tick >= BALL_TICK_DIV) { ball_tick = 0; move_ball(); }
            update_leds();

            if (check_score()) game_state = ST_GAMEOVER;
            break;

        /* ── PAUSA (0=Reanudar, 1=Salir) ───────────────────────────────── */
        case ST_PAUSE:
            if (btn & BTN_U) selected = 0;
            if (btn & BTN_D) selected = 1;
            if (btn & BTN_C) {
                game_state = (selected == 0) ? ST_PLAYING : ST_MENU;
                if (selected != 0) selected = 0;
                /* mode_2p/is_slave se limpian en after-switch cuando game_state==ST_MENU */
            }
            break;

        /* ── GAMEOVER ───────────────────────────────────────────────────── */
        case ST_GAMEOVER:
            if (btn & BTN_C) { game_state = ST_MENU; selected = 0; }
            break;
        }

        /* spi_exchange DESPUÉS del switch: ball.x/y ya fueron actualizados por
           move_ball(), así el slave recibe la posición del frame actual y no del anterior. */
        if (mode_2p && game_state != ST_WAIT_2P) {
            spi_exchange();
            if (game_state == ST_MENU) {
                /* Enviar ST_MENU por 3 frames seguidos antes de limpiar mode_2p;
                 * el slave necesita menu_consec >= 2 para salir (anti-ruido SPI). */
                if (++spi_2p_exit >= 3) { mode_2p = 0; is_slave = 0; spi_2p_exit = 0; }
            } else {
                spi_2p_exit = 0;
            }
        }

        wait_vsync();
        render_frame();
    }

    return 0;
}
