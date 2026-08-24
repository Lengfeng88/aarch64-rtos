/* dma-accel v0 HAL - register layout transcribed exactly from the
   user-provided dma_accel_regs.h (source of truth), not re-derived. */

#define REG_DEVICE_ID    0x00
#define REG_VERSION      0x04
#define REG_STATUS       0x08
#define REG_CONTROL      0x0C
#define REG_OPCODE       0x10
#define REG_SRC_ADDR_LO  0x18
#define REG_SRC_ADDR_HI  0x1C
#define REG_DST_ADDR_LO  0x20
#define REG_DST_ADDR_HI  0x24
#define REG_LEN          0x28
#define REG_CMD_ID_LO    0x2C
#define REG_CMD_ID_HI    0x30
#define REG_IRQ_STATUS   0x34
#define REG_IRQ_MASK     0x38
#define REG_ERROR_CODE   0x3C
#define REG_SQ_BASE_LO   0x40
#define REG_SQ_BASE_HI   0x44
#define REG_SQ_SIZE      0x48
#define REG_SQ_TAIL      0x4C
#define REG_CQ_BASE_LO   0x50
#define REG_CQ_BASE_HI   0x54
#define REG_CQ_SIZE      0x58
#define REG_CQ_HEAD      0x5C
#define REG_SQ_HEAD      0x60
#define REG_CQ_TAIL      0x64

#define STATUS_BUSY   (1u << 0)
#define STATUS_ERROR  (1u << 1)

#define CONTROL_START (1u << 0)
#define CONTROL_RESET (1u << 1)

#define IRQ_DMA_DONE      (1u << 0)
#define IRQ_DMA_ERROR     (1u << 1)
#define IRQ_DEVICE_ERROR  (1u << 2)

#define OPCODE_COPY        0x0
#define OPCODE_SCALE_ADD   0x1
#define OPCODE_TILE_MATMUL 0x2

#define DMA_ACCEL_QUEUE_DEPTH 16

typedef struct {
    unsigned int opcode;
    unsigned int len;
    unsigned long src_addr;
    unsigned long dst_addr;
    unsigned long cmd_id;
    unsigned long src2_addr;
    unsigned int scalar_bits;   /* same 4 bytes as the real float scalar field - avoided using an actual float type since our whole build uses -mgeneral-regs-only */
    unsigned int reserved;
} __attribute__((packed)) dma_accel_cmd_t;

typedef struct {
    unsigned long cmd_id;
    unsigned int status;
    unsigned int reserved;
} __attribute__((packed)) dma_accel_completion_t;

static volatile unsigned int *g_bar0;

/* SQ/CQ rings and cmd_id-labeled test buffers all live in plain RAM -
   the device DMAs into/out of them directly via pci_dma_read/write, our
   own CPU just needs their physical addresses. With the MMU off
   (per M1's boot setup) our own virtual addresses ARE physical
   addresses, so &array is already what the device needs. */
static dma_accel_cmd_t sq_ring[DMA_ACCEL_QUEUE_DEPTH] __attribute__((aligned(64)));
static dma_accel_completion_t cq_ring[DMA_ACCEL_QUEUE_DEPTH] __attribute__((aligned(64)));

static unsigned int sq_tail_local = 0;   /* our own producer count */
static unsigned int cq_head_local = 0;   /* our own consumer count */

static inline void reg_write(unsigned int off, unsigned int val) {
    g_bar0[off / 4] = val;
}
static inline unsigned int reg_read(unsigned int off) {
    return g_bar0[off / 4];
}

void accel_init(unsigned long bar0_phys) {
    g_bar0 = (volatile unsigned int *)bar0_phys;
}

/* Register the SQ/CQ rings with the device. Must happen once, before
   any submission - the device has no default queues to fall back to. */
void accel_setup_queues(void) {
    unsigned long sq_addr = (unsigned long)sq_ring;
    unsigned long cq_addr = (unsigned long)cq_ring;

    reg_write(REG_SQ_BASE_LO, (unsigned int)(sq_addr & 0xFFFFFFFFu));
    reg_write(REG_SQ_BASE_HI, (unsigned int)(sq_addr >> 32));
    reg_write(REG_SQ_SIZE, DMA_ACCEL_QUEUE_DEPTH);

    reg_write(REG_CQ_BASE_LO, (unsigned int)(cq_addr & 0xFFFFFFFFu));
    reg_write(REG_CQ_BASE_HI, (unsigned int)(cq_addr >> 32));
    reg_write(REG_CQ_SIZE, DMA_ACCEL_QUEUE_DEPTH);

    /* The device only asserts its IRQ line for (irq_status & irq_mask)
       - without this, DMA_DONE could set irq_status all day and the
       line would never actually raise. Poll-based bring-up never
       needed this since it just read CQ_TAIL directly, which is why
       this wasn't here before. */
    reg_write(REG_IRQ_MASK, IRQ_DMA_DONE | IRQ_DMA_ERROR | IRQ_DEVICE_ERROR);
}

static inline unsigned long irq_disable_save(void) {
    unsigned long flags;
    __asm__ volatile("mrs %0, daif" : "=r"(flags));
    __asm__ volatile("msr daifset, #2");
    return flags;
}
static inline void irq_restore(unsigned long flags) {
    __asm__ volatile("msr daif, %0" :: "r"(flags));
}

/* Submit one COPY command. Returns the cmd_id assigned (monotonic
   sq_tail_local counter - fine as long as submission itself is
   atomic, which is why the whole body is a critical section: with
   only one submitter task (M6) a preemption here could never matter,
   but M7 has several tasks submitting concurrently, and a preemption
   between computing idx and finishing the doorbell write could let
   two tasks compute the SAME idx and stomp each other's descriptor. */
unsigned long accel_submit_copy(unsigned long src, unsigned long dst,
                                 unsigned int len) {
    unsigned long flags = irq_disable_save();

    unsigned int idx = sq_tail_local % DMA_ACCEL_QUEUE_DEPTH;
    unsigned long cmd_id = sq_tail_local + 1;   /* 1-based so 0 can mean
                                                    "no completion yet" */

    sq_ring[idx].opcode = OPCODE_COPY;
    sq_ring[idx].len = len;
    sq_ring[idx].src_addr = src;
    sq_ring[idx].dst_addr = dst;
    sq_ring[idx].cmd_id = cmd_id;
    sq_ring[idx].src2_addr = 0;
    sq_ring[idx].scalar_bits = 0;
    sq_ring[idx].reserved = 0;

    sq_tail_local++;
    reg_write(REG_SQ_TAIL, sq_tail_local);   /* doorbell */

    irq_restore(flags);
    return cmd_id;
}

/* Poll-based completion check from the first bring-up pass - kept only
   as documentation of that step; the IRQ-driven path below replaces it
   for real use. */
int accel_poll_completion(dma_accel_completion_t *out, unsigned long max_spins) {
    unsigned long spins = 0;
    while (reg_read(REG_CQ_TAIL) == cq_head_local) {
        spins++;
        if (spins > max_spins) return 0;
    }
    unsigned int idx = cq_head_local % DMA_ACCEL_QUEUE_DEPTH;
    *out = cq_ring[idx];
    cq_head_local++;
    reg_write(REG_CQ_HEAD, cq_head_local);
    return 1;
}

/* IRQ-driven path. Called from the IRQ handler once IRQ_STATUS shows
   DMA_DONE. Drains every completion currently available (normally just
   one, for a single-outstanding-command demo, but a real driver could
   see several batched together) and remembers the most recent one -
   good enough for a single in-flight command; a real driver would
   dispatch each by cmd_id to whichever waiter is actually waiting on
   it. Returns how many were drained, so the caller knows how many
   times to wake a waiter. */
static dma_accel_completion_t last_completion;

int accel_drain_completions(void) {
    int drained = 0;
    while (reg_read(REG_CQ_TAIL) != cq_head_local) {
        unsigned int idx = cq_head_local % DMA_ACCEL_QUEUE_DEPTH;
        last_completion = cq_ring[idx];
        cq_head_local++;
        reg_write(REG_CQ_HEAD, cq_head_local);
        drained++;
    }
    return drained;
}

/* M7: multiple commands can genuinely be in flight at once (the device
   model supports up to DMA_ACCEL_MAX_INFLIGHT concurrently), so a
   single "last completion" slot isn't enough - one IRQ can legitimately
   need to deliver several different commands' results to several
   different waiting tasks. Drains up to max_out completions into the
   caller's array and returns how many were actually drained. */
int accel_drain_completions_multi(dma_accel_completion_t *out, int max_out) {
    int drained = 0;
    while (drained < max_out && reg_read(REG_CQ_TAIL) != cq_head_local) {
        unsigned int idx = cq_head_local % DMA_ACCEL_QUEUE_DEPTH;
        out[drained] = cq_ring[idx];
        cq_head_local++;
        reg_write(REG_CQ_HEAD, cq_head_local);
        drained++;
    }
    return drained;
}

dma_accel_completion_t accel_last_completion(void) {
    return last_completion;
}

unsigned int accel_irq_status(void) {
    return reg_read(REG_IRQ_STATUS);
}

void accel_irq_ack(unsigned int bits) {
    reg_write(REG_IRQ_STATUS, bits);   /* W1C per the device spec */
}

/* Debug-only: peek at the device's raw CQ_TAIL and our own local
   cq_head, bypassing the IRQ path entirely, to tell apart "the device
   hasn't produced the completion yet" from "it has, but we were never
   interrupted about it." */
unsigned int accel_debug_cq_tail(void) {
    return reg_read(REG_CQ_TAIL);
}
unsigned int accel_debug_cq_head_local(void) {
    return cq_head_local;
}