/*
 * Micro OS — the x86-64 port: the legacy interface of virtio.
 *
 * virtio is the way this port speaks to a device of the PCI bus, and it has
 * two shapes: the modern one of virtio 1.0, whose registers are a capability
 * the configuration space names, and the legacy one, whose register file is
 * an I/O window of the card. The port speaks the second, and for a reason
 * that is about this machine and not about taste: the kernel of the port is
 * the identity map of the first gigabyte (ADR 0032), the MMIO window of a
 * device of QEMU sits near four gigabytes, and a driver that needs it would
 * need a mapping of MMIO in every address space of the machine before it
 * could touch its first register. An I/O window is a port number.
 *
 * The rings are the same in both shapes: the split virtqueue of the
 * specification, a table of sixteen-byte descriptors, an available ring the
 * driver writes and a used ring the device writes, and the two of them
 * named by the page number of the table. This file is that structure and
 * the accesses of the window, and it is shared by the two devices of the
 * port on the bus (src/virtio_net.c and src/virtio_blk.c) so that the
 * second driver is a user of the first one and not a second copy of the
 * same ring.
 *
 * What is not here is the policy of a driver: which features it negotiates,
 * which descriptors it builds and how long it waits. A network card sends a
 * frame and a disk moves sectors; the ring they share has no opinion about
 * either.
 */
#ifndef MICRO_OS_PORT_X86_64_VIRTIO_H
#define MICRO_OS_PORT_X86_64_VIRTIO_H

#include "kernel/types.h"

/* --- the register file of a legacy virtio device in its I/O BAR ----------- */

/*
 * The registers of the window. The modern interface replaces them with a
 * capability in the configuration space; this driver asks QEMU for a
 * transitional device, whose legacy window is always there, and a machine
 * that offers only the modern one is named in the log instead of silently
 * left without a device.
 */
#define VIRTIO_PCI_DEVICE_FEATURES 0x00u
#define VIRTIO_PCI_DRIVER_FEATURES 0x04u
#define VIRTIO_PCI_QUEUE_ADDRESS 0x08u
#define VIRTIO_PCI_QUEUE_SIZE 0x0Cu
#define VIRTIO_PCI_QUEUE_SELECT 0x0Eu
#define VIRTIO_PCI_QUEUE_NOTIFY 0x10u
#define VIRTIO_PCI_STATUS 0x12u
#define VIRTIO_PCI_ISR 0x13u
#define VIRTIO_PCI_CONFIG 0x14u

/* The four states of the handshake, and the one that gives up. */
#define VIRTIO_STATUS_ACKNOWLEDGE 0x01u
#define VIRTIO_STATUS_DRIVER 0x02u
#define VIRTIO_STATUS_DRIVER_OK 0x04u
#define VIRTIO_STATUS_FAILED 0x80u

/* The two bits of a descriptor: the next one of the chain, and "the device
 * writes this buffer" - a receive, a read of a disk - which is the opposite
 * of the direction the words are used for. */
#define VIRTQ_DESC_F_NEXT 0x0001u
#define VIRTQ_DESC_F_WRITE 0x0002u

/* The two flags that keep the device quiet: no interrupt for an entry the
 * driver has not asked about, and no notification for a buffer that came
 * back. */
#define VIRTQ_AVAIL_F_NO_INTERRUPT 0x0001u
#define VIRTQ_USED_F_NO_NOTIFY 0x0001u

/* The largest queue this port sets up. The legacy interface fixes the size
 * of a queue at the boot and it is a power of two; QEMU answers 128 or 256
 * for the devices of this machine, and a bigger ring is memory a driver of
 * one task has no use for. */
#define VIRTIO_QUEUE_MAX 256u

/*
 * One queue of a device: the split ring and the cursor of the driver in the
 * used half. The memory is one page-aligned block with the three rings in
 * it, laid out the way the legacy interface fixes; it lives as long as the
 * device does and is never given back.
 */
struct virtqueue {
    k_u16_t size;
    k_u8_t *desc;      /* size entries of sixteen bytes */
    k_u8_t *avail;     /* flags, idx, ring[size], used_event */
    k_u8_t *used;      /* flags, idx, ring[size] of {id, len}, avail_event */
    k_u16_t last_used; /* the used entry the driver has not looked at yet */
    k_u8_t *memory;    /* the allocation behind the rings, page aligned */
};

/* --- the window, one access at a time ------------------------------------- */

/*
 * The width of an access is the width of the register and not a choice of
 * the caller: the legacy interface fixes it - a byte for the status, a word
 * for the queue of a notification, a long for the features - and a wider
 * read of a byte register is not a number with the byte in it.
 */
k_u8_t virtio_in8(k_u16_t base, k_u16_t offset);
k_u16_t virtio_in16(k_u16_t base, k_u16_t offset);
k_u32_t virtio_in32(k_u16_t base, k_u16_t offset);
void virtio_out8(k_u16_t base, k_u16_t offset, k_u8_t value);
void virtio_out16(k_u16_t base, k_u16_t offset, k_u16_t value);
void virtio_out32(k_u16_t base, k_u16_t offset, k_u32_t value);

/*
 * The stores of a driver must be in the memory before the device is told to
 * look at them. The processor of this machine does not reorder them, but the
 * compiler may, and this is the line that stops it.
 */
void virtio_barrier(void);

/* Write the status register; zero is the reset of the device. */
void virtio_status(k_u16_t base, k_u8_t status);

/*
 * The reset handshake: a zero in the status register, and the device may take
 * a moment to honour it. False when it did not: a device that answers nothing
 * is a device the driver leaves alone.
 */
bool virtio_reset(k_u16_t base);

/* Tell the device that the queue has work in it. */
void virtio_notify(k_u16_t base, k_u16_t queue);

/*
 * Bring one queue up: select it, learn the size from the device (read-only in
 * the legacy interface), build the ring and give the device the page number
 * of the descriptor table. `interrupts` says whether the driver wants the
 * line of the device when an entry comes back; a driver that polls the used
 * ring sets it false and the device stays quiet.
 */
bool virtio_queue_setup(k_u16_t base, struct virtqueue *queue, k_u16_t index, bool interrupts);

/* One entry of the descriptor table. */
void virtqueue_descriptor(struct virtqueue *queue, k_u16_t index, k_u64_t address, k_u32_t length,
                          k_u16_t flags, k_u16_t next);

/* Make the chain that starts at `head` available to the device. */
void virtqueue_avail_push(struct virtqueue *queue, k_u16_t head);

/* The used index the device has reached, and one entry of the used ring. */
k_u16_t virtqueue_used_index(const struct virtqueue *queue);
void virtqueue_used_entry(const struct virtqueue *queue, k_u16_t index, k_u32_t *id,
                          k_u32_t *length);

#endif /* MICRO_OS_PORT_X86_64_VIRTIO_H */
