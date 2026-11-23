/*
 * Micro OS — the x86-64 port: the split virtqueue of a legacy virtio device.
 *
 * One queue is a page-aligned block of memory with three rings in it: the
 * table of the descriptors, the available ring the driver writes, and the
 * used ring the device writes. The legacy interface does not let the driver
 * choose the layout - the descriptor table starts at the base, the available
 * ring follows it, and the used ring starts at the next page boundary - and
 * it does not let the driver choose the size either: the queue is the
 * device's, and a write of the size register is ignored.
 *
 * The ring is polled by whoever owns it and the memory is the memory of the
 * kernel, which is the identity map of this port: the address of a ring is
 * the address the device wants. A kernel that moves to a higher half asks
 * the paging code for the translation here, and these three rings are the
 * only memory of a virtio driver that does.
 */
#include "port_x86_64.h"

#include <string.h>

#include "kernel/port/mem.h"
#include "kernel/port/time.h"
#include "port_x86_64_virtio.h"

/* --- the window ----------------------------------------------------------- */

k_u8_t virtio_in8(k_u16_t base, k_u16_t offset)
{
    return port_inb((k_u16_t)(base + offset));
}

k_u16_t virtio_in16(k_u16_t base, k_u16_t offset)
{
    return port_inw((k_u16_t)(base + offset));
}

k_u32_t virtio_in32(k_u16_t base, k_u16_t offset)
{
    return port_inl((k_u16_t)(base + offset));
}

void virtio_out8(k_u16_t base, k_u16_t offset, k_u8_t value)
{
    port_outb((k_u16_t)(base + offset), value);
}

void virtio_out16(k_u16_t base, k_u16_t offset, k_u16_t value)
{
    port_outw((k_u16_t)(base + offset), value);
}

void virtio_out32(k_u16_t base, k_u16_t offset, k_u32_t value)
{
    port_outl((k_u16_t)(base + offset), value);
}

void virtio_barrier(void)
{
    __asm__ volatile("" : : : "memory");
}

void virtio_status(k_u16_t base, k_u8_t status)
{
    virtio_out8(base, VIRTIO_PCI_STATUS, status);
}

/* Reading the interrupt status is what takes the line of the device down: a
 * driver that never reads it is a line that stays up and a storm of
 * interrupts. */
void virtio_notify(k_u16_t base, k_u16_t queue)
{
    virtio_out16(base, VIRTIO_PCI_QUEUE_NOTIFY, queue);
}

/*
 * A reset is a zero in the status register, and the device may take a moment
 * to honour it: the loop is the handshake of the legacy interface. A device
 * that does not take it in the budget is a device without a driver.
 */
bool virtio_reset(k_u16_t base)
{
    k_u64_t deadline = k_port_uptime_ms() + 100u;

    virtio_status(base, 0);

    while (virtio_in8(base, VIRTIO_PCI_STATUS) != 0u) {
        if (k_port_uptime_ms() > deadline) {
            return false;
        }
    }

    return true;
}

/* --- the rings ------------------------------------------------------------ */

/* The offset of the used ring behind the base, which is the first page after
 * the descriptor table and the available ring. */
static k_u64_t vq_used_offset(k_u16_t size)
{
    k_u64_t after_avail = (k_u64_t)size * 16u + 6u + (k_u64_t)size * 2u;

    return (after_avail + 4095u) & ~(k_u64_t)4095u;
}

/*
 * The layout the legacy interface fixes, in one block whose first byte is on
 * a page boundary. The allocation is one page bigger than the rings, because
 * the manager of the memory of the port returns a block aligned to sixteen
 * bytes and a ring has to be aligned to a page.
 */
static bool vq_allocate(struct virtqueue *queue, k_u16_t size)
{
    k_u64_t used_offset = vq_used_offset(size);
    k_size_t total = (k_size_t)(used_offset + 6u + (k_u64_t)size * 8u);
    k_u8_t *raw = (k_u8_t *)k_port_alloc(total + 4096u, false);
    k_u8_t *aligned;

    if (raw == NULL) {
        return false;
    }

    aligned = (k_u8_t *)(((k_u64_t)(uintptr_t)raw + 4095u) & ~(k_u64_t)4095u);

    memset(aligned, 0, total);

    queue->size = size;
    queue->memory = raw;
    queue->desc = aligned;
    queue->avail = aligned + (k_size_t)size * 16u;
    queue->used = aligned + (k_size_t)used_offset;
    queue->last_used = 0;

    return true;
}

bool virtio_queue_setup(k_u16_t base, struct virtqueue *queue, k_u16_t index, bool interrupts)
{
    k_u16_t size;
    k_u64_t phys;

    virtio_out16(base, VIRTIO_PCI_QUEUE_SELECT, index);
    size = virtio_in16(base, VIRTIO_PCI_QUEUE_SIZE);

    if (size < 2u || size > VIRTIO_QUEUE_MAX) {
        return false;
    }

    /* The ring of a device is built once and kept: a driver that resets its
     * device after a failure programs the same memory again instead of
     * leaving a page of the kernel behind for every recovery. */
    if (queue->memory == NULL) {
        if (!vq_allocate(queue, size)) {
            return false;
        }
    } else if (queue->size != size) {
        return false;
    }

    /*
     * The two halves of the quiet: a driver that polls the used ring asks
     * the device for no interrupt, and a driver that waits for one asks for
     * every entry. The notification of the other direction is never wanted
     * here - the driver looks at the used ring on its own - so the used ring
     * says so from the first byte.
     *
     * The two indexes start at zero: the memory of the ring is the driver's,
     * and a device that was reset begins at the first entry of it.
     */
    queue->avail[0] = interrupts ? 0u : (k_u8_t)VIRTQ_AVAIL_F_NO_INTERRUPT;
    queue->avail[1] = 0;
    queue->avail[2] = 0;
    queue->avail[3] = 0;
    queue->used[0] = (k_u8_t)VIRTQ_USED_F_NO_NOTIFY;
    queue->used[1] = 0;
    queue->used[2] = 0;
    queue->used[3] = 0;
    queue->last_used = 0;

    phys = port_x86_64_bus_phys(queue->desc);

    virtio_barrier();
    virtio_out32(base, VIRTIO_PCI_QUEUE_ADDRESS, (k_u32_t)(phys >> 12));

    return true;
}

void virtqueue_descriptor(struct virtqueue *queue, k_u16_t index, k_u64_t address, k_u32_t length,
                          k_u16_t flags, k_u16_t next)
{
    k_u8_t *desc = queue->desc + (k_size_t)index * 16u;

    desc[0] = (k_u8_t)(address & 0xFFu);
    desc[1] = (k_u8_t)((address >> 8) & 0xFFu);
    desc[2] = (k_u8_t)((address >> 16) & 0xFFu);
    desc[3] = (k_u8_t)((address >> 24) & 0xFFu);
    desc[4] = (k_u8_t)((address >> 32) & 0xFFu);
    desc[5] = (k_u8_t)((address >> 40) & 0xFFu);
    desc[6] = (k_u8_t)((address >> 48) & 0xFFu);
    desc[7] = (k_u8_t)((address >> 56) & 0xFFu);

    desc[8] = (k_u8_t)(length & 0xFFu);
    desc[9] = (k_u8_t)((length >> 8) & 0xFFu);
    desc[10] = (k_u8_t)((length >> 16) & 0xFFu);
    desc[11] = (k_u8_t)((length >> 24) & 0xFFu);

    desc[12] = (k_u8_t)(flags & 0xFFu);
    desc[13] = (k_u8_t)((flags >> 8) & 0xFFu);
    desc[14] = (k_u8_t)(next & 0xFFu);
    desc[15] = (k_u8_t)((next >> 8) & 0xFFu);
}

static k_u16_t vq_avail_index(const struct virtqueue *queue)
{
    return (k_u16_t)(queue->avail[2] | ((k_u16_t)queue->avail[3] << 8));
}

static void vq_avail_set_index(struct virtqueue *queue, k_u16_t value)
{
    queue->avail[2] = (k_u8_t)(value & 0xFFu);
    queue->avail[3] = (k_u8_t)(value >> 8);
}

void virtqueue_avail_push(struct virtqueue *queue, k_u16_t head)
{
    k_u16_t index = vq_avail_index(queue);
    k_u8_t *slot = queue->avail + 4u + (k_size_t)(index % queue->size) * 2u;

    slot[0] = (k_u8_t)(head & 0xFFu);
    slot[1] = (k_u8_t)(head >> 8);

    /* The index is the device's signal that a new entry is there, and it
     * moves after the entry itself. */
    virtio_barrier();
    vq_avail_set_index(queue, (k_u16_t)(index + 1u));
}

k_u16_t virtqueue_used_index(const struct virtqueue *queue)
{
    return (k_u16_t)(queue->used[2] | ((k_u16_t)queue->used[3] << 8));
}

void virtqueue_used_entry(const struct virtqueue *queue, k_u16_t index, k_u32_t *id,
                          k_u32_t *length)
{
    const k_u8_t *entry = queue->used + 4u + (k_size_t)(index % queue->size) * 8u;

    *id = (k_u32_t)entry[0] | ((k_u32_t)entry[1] << 8) | ((k_u32_t)entry[2] << 16) |
          ((k_u32_t)entry[3] << 24);
    *length = (k_u32_t)entry[4] | ((k_u32_t)entry[5] << 8) | ((k_u32_t)entry[6] << 16) |
              ((k_u32_t)entry[7] << 24);
}
