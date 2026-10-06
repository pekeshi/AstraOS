#include "xhci.h"

#include "pmm.h"

#include <stdint.h>

#define PAGE_SIZE 4096U
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC
#define XHCI_RING_TRBS 256U
#define XHCI_EVENT_TRBS 256U
#define XHCI_WAIT_LIMIT 4000000U
#define XHCI_MICROFRAME_WAIT_LIMIT 20000000U
#define XHCI_PORT_DEBOUNCE_MICROFRAMES 800U
#define TRB_CYCLE 1U
#define TRB_TOGGLE_CYCLE (1U << 1)
#define TRB_CHAIN (1U << 4)
#define TRB_INTERRUPT (1U << 5)

struct xhci_trb {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
};

struct xhci_erst_entry {
    uint64_t ring_segment_base;
    uint32_t ring_segment_size;
    uint32_t reserved;
};

struct xhci_ring {
    volatile struct xhci_trb *trbs;
    uint64_t physical;
    uint32_t index;
    uint32_t cycle;
};

struct xhci_controller {
    volatile uint32_t *operational;
    volatile uint32_t *doorbells;
    volatile uint32_t *runtime_base;
    volatile uint32_t *runtime;
    uint32_t max_ports;
    uint32_t max_slots;
    uint32_t context_size;
    uint32_t port_power_control;
    uint32_t max_packet0;
    uint32_t supports_64bit_dma;
    uint32_t scratchpad_count;
    uint64_t dcbaa_physical;
    uint64_t event_ring_physical;
    uint64_t erst_physical;
    uint64_t scratchpad_array_physical;
    uint32_t event_index;
    uint32_t event_cycle;
    uint32_t slot_id;
    uint32_t endpoint_id;
    uint32_t keyboard_port;
    uint32_t report_length;
    uint32_t keyboard_pending;
    uint32_t previous_keys[6];
    uint8_t caps_lock;
    uint8_t *report;
    uint8_t *device_context;
    uint8_t *input_context;
    uint64_t *device_context_array;
    struct xhci_ring command_ring;
    struct xhci_ring control_ring;
    struct xhci_ring interrupt_ring;
    struct xhci_trb *event_ring;
};

static struct xhci_controller controller;
static struct xhci_diagnostics diagnostics;
static struct xhci_diagnostics
    controller_diagnostics[XHCI_MAX_DIAGNOSTIC_CONTROLLERS];
static uint32_t controller_diagnostic_count;
static uint32_t pci_scan_index;
static int pci_xhci_found;

static inline void io_out32(uint16_t port, uint32_t value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline void io_out16(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t io_in32(uint16_t port) {
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint32_t pci_read32(uint8_t bus, uint8_t device, uint8_t function,
                           uint8_t offset) {
    uint32_t address = 0x80000000U | ((uint32_t)bus << 16) |
                       ((uint32_t)device << 11) | ((uint32_t)function << 8) |
                       (offset & 0xFC);
    io_out32(PCI_CONFIG_ADDRESS, address);
    return io_in32(PCI_CONFIG_DATA);
}

static void pci_write16(uint8_t bus, uint8_t device, uint8_t function,
                        uint8_t offset, uint16_t value) {
    uint32_t address = 0x80000000U | ((uint32_t)bus << 16) |
                       ((uint32_t)device << 11) | ((uint32_t)function << 8) |
                       (offset & 0xFC);

    io_out32(PCI_CONFIG_ADDRESS, address);
    io_out16((uint16_t)(PCI_CONFIG_DATA + (offset & 2)), value);
}

static uint64_t allocate_page(void) {
    uint64_t physical = pmm_alloc_page();

    if (physical != 0) {
        if (!controller.supports_64bit_dma && physical > UINT32_MAX) {
            pmm_free_page(physical);
            return 0;
        }
        volatile uint64_t *words = (volatile uint64_t *)(uintptr_t)physical;
        for (uint32_t i = 0; i < PAGE_SIZE / sizeof(*words); ++i) {
            words[i] = 0;
        }
    }
    return physical;
}

static uint32_t read_reg(volatile uint32_t *address) {
    return *address;
}

static void write_reg(volatile uint32_t *address, uint32_t value) {
    *address = value;
}

static int wait_reg(volatile uint32_t *address, uint32_t mask,
                    uint32_t expected) {
    for (uint32_t i = 0; i < XHCI_WAIT_LIMIT; ++i) {
        if ((read_reg(address) & mask) == expected) {
            return 1;
        }
    }
    return 0;
}

static int wait_microframes(uint32_t microframes) {
    uint32_t start = read_reg(controller.runtime_base);

    for (uint32_t i = 0; i < XHCI_MICROFRAME_WAIT_LIMIT; ++i) {
        uint32_t current = read_reg(controller.runtime_base);

        if (((current - start) & 0x3FFFU) >= microframes) {
            return 1;
        }
    }
    return 0;
}

static volatile struct xhci_trb *event_at(uint32_t index) {
    return &controller.event_ring[index];
}

static int pop_event(struct xhci_trb *result) {
    volatile struct xhci_trb *event = event_at(controller.event_index);
    uint32_t control = event->control;

    if ((control & TRB_CYCLE) != controller.event_cycle) {
        return 0;
    }
    result->parameter = event->parameter;
    result->status = event->status;
    result->control = control;
    ++controller.event_index;
    if (controller.event_index == XHCI_EVENT_TRBS) {
        controller.event_index = 0;
        controller.event_cycle ^= 1;
    }
    controller.runtime[0x18 / sizeof(uint32_t)] =
        (uint32_t)((uintptr_t)event_at(controller.event_index) | 8U);
    return 1;
}

static int next_event(uint32_t type, uint32_t *completion_code,
                      struct xhci_trb *captured) {
    for (uint32_t i = 0; i < XHCI_WAIT_LIMIT; ++i) {
        struct xhci_trb event;

        if (!pop_event(&event)) {
            continue;
        }
        if (((event.control >> 10) & 0x3F) != type) {
            continue;
        }
        if (completion_code != 0) {
            *completion_code = (event.status >> 24) & 0xFF;
        }
        if (captured != 0) {
            *captured = event;
        }
        return 1;
    }
    return 0;
}

static void ring_doorbell(uint32_t target, uint32_t stream_id) {
    controller.doorbells[target] = stream_id;
}

static void ring_push(struct xhci_ring *ring, const struct xhci_trb *trb) {
    volatile struct xhci_trb *link;
    struct xhci_trb entry = *trb;

    if (ring->index == XHCI_RING_TRBS - 1) {
        link = &ring->trbs[ring->index];
        link->parameter = ring->physical;
        link->status = 0;
        link->control = (6U << 10) | TRB_TOGGLE_CYCLE | ring->cycle;
        ring->index = 0;
        ring->cycle ^= 1;
    }
    entry.control = (entry.control & ~TRB_CYCLE) | ring->cycle;
    ring->trbs[ring->index].parameter = entry.parameter;
    ring->trbs[ring->index].status = entry.status;
    ring->trbs[ring->index].control = entry.control;
    ++ring->index;
}

static void ring_init(struct xhci_ring *ring, uint64_t physical) {
    ring->trbs = (volatile struct xhci_trb *)(uintptr_t)physical;
    ring->physical = physical;
    ring->index = 0;
    ring->cycle = 1;
}

static uint32_t context_dword(const uint8_t *context, uint32_t index) {
    const uint32_t *dwords = (const uint32_t *)(const void *)context;
    return dwords[index];
}

static void set_context_dword(uint8_t *context, uint32_t index,
                              uint32_t value) {
    uint32_t *dwords = (uint32_t *)(void *)context;
    dwords[index] = value;
}

static uint8_t *input_context(uint32_t context_index) {
    return controller.input_context +
           (uint64_t)context_index * controller.context_size;
}

static uint8_t *output_context(uint32_t context_index) {
    return controller.device_context +
           (uint64_t)context_index * controller.context_size;
}

static int wait_command(uint32_t *slot_out) {
    uint32_t completion;
    struct xhci_trb event;

    if (!next_event(33, &completion, &event)) {
        return 0;
    }
    diagnostics.last_completion_code = completion;
    if (completion != 1) {
        return 0;
    }
    if (slot_out != 0) {
        *slot_out = event.control >> 24;
    }
    return 1;
}

static int command(uint32_t type, uint64_t parameter, uint32_t status,
                   uint32_t control, uint32_t *slot_out) {
    struct xhci_trb trb;

    diagnostics.last_command_type = type;
    diagnostics.last_completion_code = 0;
    trb.parameter = parameter;
    trb.status = status;
    trb.control = (type << 10) | control;
    ring_push(&controller.command_ring, &trb);
    ring_doorbell(0, 0);
    return wait_command(slot_out);
}

static int wait_transfer(uint32_t endpoint, uint32_t *completion,
                         uint32_t *residual) {
    struct xhci_trb event;

    if (!next_event(32, completion, &event) ||
        (event.control >> 24) != controller.slot_id ||
        ((event.control >> 16) & 0x1F) != endpoint) {
        return 0;
    }
    *residual = event.status & 0xFFFFFF;
    return 1;
}

static int poll_transfer(uint32_t endpoint, uint32_t *completion,
                         uint32_t *residual) {
    struct xhci_trb event;

    while (pop_event(&event)) {
        if (((event.control >> 10) & 0x3F) != 32 ||
            (event.control >> 24) != controller.slot_id ||
            ((event.control >> 16) & 0x1F) != endpoint) {
            continue;
        }
        *completion = (event.status >> 24) & 0xFF;
        *residual = event.status & 0xFFFFFF;
        return 1;
    }
    return 0;
}

static int control_transfer(uint8_t request_type, uint8_t request,
                            uint16_t value, uint16_t index,
                            void *data, uint16_t length) {
    struct xhci_trb trb;
    uint32_t completion;
    uint32_t residual;
    uint64_t setup = (uint64_t)request_type |
                     ((uint64_t)request << 8) |
                     ((uint64_t)value << 16) |
                     ((uint64_t)index << 32) |
                     ((uint64_t)length << 48);

    trb.parameter = setup;
    trb.status = 8;
    trb.control = (2U << 10) | (1U << 6) | TRB_CHAIN;
    if (length != 0) {
        trb.control |= ((request_type & 0x80) != 0 ? 3U : 2U) << 16;
    }
    ring_push(&controller.control_ring, &trb);
    if (length != 0) {
        trb.parameter = (uint64_t)(uintptr_t)data;
        trb.status = length;
        trb.control = (3U << 10) | TRB_CHAIN;
        if ((request_type & 0x80) != 0) {
            trb.control |= 1U << 16;
        }
        ring_push(&controller.control_ring, &trb);
    }
    trb.parameter = 0;
    trb.status = 0;
    trb.control = (4U << 10) | TRB_INTERRUPT;
    if (length == 0 || (request_type & 0x80) == 0) {
        trb.control |= 1U << 16;
    }
    ring_push(&controller.control_ring, &trb);
    ring_doorbell(controller.slot_id, 1);
    return wait_transfer(1, &completion, &residual) && completion == 1 &&
           residual <= length;
}

static int set_address(uint8_t speed, uint32_t port) {
    uint8_t *ep0_context = input_context(2);
    uint32_t completion_slot = 0;

    for (uint32_t i = 0; i < 8; ++i) {
        set_context_dword(input_context(0), i, 0);
        set_context_dword(input_context(1), i, 0);
        set_context_dword(ep0_context, i, 0);
    }
    set_context_dword(input_context(0), 1, (1U << 0) | (1U << 1));
    set_context_dword(input_context(1), 0, (speed << 20) | (1U << 27));
    set_context_dword(input_context(1), 1, port << 16);
    controller.max_packet0 = speed == 3 ? 64 : (speed >= 4 ? 512 : 8);
    set_context_dword(ep0_context, 1, (3U << 1) | (4U << 3) |
                      (controller.max_packet0 << 16));
    set_context_dword(ep0_context, 2,
                      (uint32_t)controller.control_ring.physical | 1U);
    set_context_dword(ep0_context, 3,
                      (uint32_t)(controller.control_ring.physical >> 32));
    set_context_dword(ep0_context, 4, 8);

    if (controller.device_context_array == 0 || controller.slot_id == 0) {
        return 0;
    }
    if (!command(11, (uint64_t)(uintptr_t)controller.input_context, 0,
                 controller.slot_id << 24, &completion_slot) ||
        completion_slot == 0) {
        return 0;
    }
    controller.slot_id = completion_slot;
    return 1;
}

static int update_ep0_packet_size(uint32_t packet_size) {
    uint8_t *out_slot = output_context(0);
    uint8_t *out_ep0 = output_context(1);
    uint8_t *in_slot = input_context(1);
    uint8_t *in_ep0 = input_context(2);

    for (uint32_t i = 0; i < controller.context_size / sizeof(uint32_t); ++i) {
        set_context_dword(in_slot, i, context_dword(out_slot, i));
        set_context_dword(in_ep0, i, context_dword(out_ep0, i));
    }
    set_context_dword(input_context(0), 0, 0);
    set_context_dword(input_context(0), 1, (1U << 0) | (1U << 1));
    set_context_dword(in_ep0, 1,
                      (context_dword(in_ep0, 1) & 0x0000FFFFU) |
                      (packet_size << 16));
    return command(13, (uint64_t)(uintptr_t)controller.input_context,
                   0, controller.slot_id << 24, 0);
}

static int setup_interrupt_endpoint(uint8_t endpoint_address,
                                   uint16_t max_packet, uint8_t interval) {
    uint32_t endpoint_number = endpoint_address & 0x0F;
    uint8_t *slot_context = input_context(1);
    uint8_t *ep0_context = input_context(2);
    uint8_t *endpoint_context;
    uint32_t endpoint_type = (endpoint_address & 0x80) != 0 ? 7 : 3;
    uint32_t packet = max_packet & 0x7FF;

    if (endpoint_number == 0 || packet < 8 || packet > 1024) {
        return 0;
    }
    controller.endpoint_id = endpoint_number * 2 + 1;
    if (controller.endpoint_id >= 32) {
        return 0;
    }
    endpoint_context = input_context(controller.endpoint_id + 1);
    for (uint32_t i = 0; i < controller.context_size / sizeof(uint32_t); ++i) {
        set_context_dword(slot_context, i, context_dword(output_context(0), i));
        set_context_dword(ep0_context, i, context_dword(output_context(1), i));
        set_context_dword(endpoint_context, i, 0);
    }
    set_context_dword(input_context(0), 0, 0);
    set_context_dword(input_context(0), 1,
                      (1U << 0) |
                      (1U << controller.endpoint_id));
    set_context_dword(slot_context, 0,
                      (context_dword(slot_context, 0) & ~(0x1FU << 27)) |
                      ((controller.endpoint_id & 0x1F) << 27));
    set_context_dword(endpoint_context, 0, (uint32_t)interval << 16);
    set_context_dword(endpoint_context, 1, (3U << 1) | (endpoint_type << 3) |
                      (packet << 16));
    set_context_dword(endpoint_context, 2,
                      (uint32_t)controller.interrupt_ring.physical | 1U);
    set_context_dword(endpoint_context, 3,
                      (uint32_t)(controller.interrupt_ring.physical >> 32));
    set_context_dword(endpoint_context, 4, packet);
    if (!command(12, (uint64_t)(uintptr_t)controller.input_context, 0,
                 controller.slot_id << 24, 0)) {
        return 0;
    }
    controller.report_length = packet;
    return 1;
}

static void release_keyboard_candidate(void) {
    uint32_t last_command_type = diagnostics.last_command_type;
    uint32_t last_completion_code = diagnostics.last_completion_code;

    if (controller.slot_id != 0) {
        controller.device_context_array[controller.slot_id] = 0;
        (void)command(10, 0, 0, controller.slot_id << 24, 0);
    }
    if (controller.control_ring.physical != 0) {
        pmm_free_page(controller.control_ring.physical);
    }
    if (controller.interrupt_ring.physical != 0) {
        pmm_free_page(controller.interrupt_ring.physical);
    }
    if (controller.report != 0) {
        pmm_free_page((uint64_t)(uintptr_t)controller.report);
    }
    if (controller.input_context != 0) {
        pmm_free_page((uint64_t)(uintptr_t)controller.input_context);
    }
    if (controller.device_context != 0) {
        pmm_free_page((uint64_t)(uintptr_t)controller.device_context);
    }
    controller.slot_id = 0;
    controller.report = 0;
    controller.input_context = 0;
    controller.device_context = 0;
    controller.control_ring.physical = 0;
    controller.interrupt_ring.physical = 0;
    diagnostics.last_command_type = last_command_type;
    diagnostics.last_completion_code = last_completion_code;
}

static uint32_t read_portsc(uint32_t port) {
    return read_reg(controller.operational + (0x400 / 4) + (port - 1) * 4);
}

static void write_portsc(uint32_t port, uint32_t value) {
    write_reg(controller.operational + (0x400 / 4) + (port - 1) * 4, value);
}

static void set_port_stage(uint32_t port, uint32_t stage) {
    diagnostics.port_stage = stage;
    if (port != 0 && port <= XHCI_MAX_PORTS) {
        diagnostics.port_stages[port - 1] = stage;
    }
}

static int reset_port(uint32_t port, uint32_t *speed_out) {
    uint32_t status = read_portsc(port);
    int reset_complete = 0;

    if ((status & 1U) == 0) {
        set_port_stage(port, XHCI_PORT_STAGE_NO_CONNECTION);
        return 0;
    }
    set_port_stage(port, XHCI_PORT_STAGE_DEBOUNCE);
    if (!wait_microframes(XHCI_PORT_DEBOUNCE_MICROFRAMES)) {
        return 0;
    }
    status = read_portsc(port);
    if ((status & 1U) == 0) {
        set_port_stage(port, XHCI_PORT_STAGE_CONNECTION_LOST);
        return 0;
    }
    if ((status & (1U << 9)) == 0) {
        write_portsc(port, 1U << 9);
    }
    for (volatile uint32_t delay = 0; delay < 100000; ++delay) {
    }
    status = read_portsc(port);
    *speed_out = (status >> 10) & 0x0F;
    set_port_stage(port, XHCI_PORT_STAGE_RESET);
    if (*speed_out >= 4) {
        write_portsc(port, (1U << 9) | (1U << 31) |
                     (status & (0x7FU << 17)));
        for (uint32_t i = 0; i < XHCI_WAIT_LIMIT; ++i) {
            status = read_portsc(port);
            if ((status & (1U << 19)) != 0) {
                write_portsc(port, (1U << 9) | (1U << 19));
                reset_complete = 1;
                break;
            }
        }
    } else {
        write_portsc(port, (1U << 9) | (1U << 4) |
                     (status & (0x7FU << 17)));
        for (uint32_t i = 0; i < XHCI_WAIT_LIMIT; ++i) {
            status = read_portsc(port);
            if ((status & (1U << 21)) != 0) {
                write_portsc(port, (1U << 9) | (1U << 21));
                reset_complete = 1;
                break;
            }
        }
    }
    status = read_portsc(port);
    *speed_out = (status >> 10) & 0x0F;
    if (!reset_complete) {
        set_port_stage(port, XHCI_PORT_STAGE_RESET_TIMEOUT);
        return 0;
    }
    if ((status & 3U) != 3U || *speed_out == 0) {
        set_port_stage(port, XHCI_PORT_STAGE_NOT_ENABLED);
        return 0;
    }
    set_port_stage(port, XHCI_PORT_STAGE_READY);
    return 1;
}

static int usb_find_keyboard(uint32_t port, uint32_t speed) {
    uint8_t device_descriptor[18];
    uint8_t configuration[256];
    uint32_t slot;
    uint32_t endpoint_address = 0;
    uint32_t endpoint_packet = 0;
    uint32_t endpoint_interval = 0;
    uint32_t interface_number = 0;
    uint32_t configuration_value = 0;
    uint32_t total_length;
    uint32_t interface_is_keyboard = 0;
    uint32_t max_packet0;
    uint64_t device_context;
    uint64_t input_context_address;
    uint64_t ring;
    uint64_t report;
    uint32_t descriptor_offset;

    diagnostics.enumeration_stage = 1;
    if (!command(9, 0, 0, 0, &slot) || slot == 0 ||
        slot > controller.max_slots) {
        return 0;
    }
    controller.slot_id = slot;
    device_context = allocate_page();
    input_context_address = allocate_page();
    ring = allocate_page();
    report = allocate_page();
    if (device_context == 0 || input_context_address == 0 ||
        ring == 0 || report == 0) {
        diagnostics.enumeration_stage = 2;
        if (device_context != 0) {
            pmm_free_page(device_context);
        }
        if (input_context_address != 0) {
            pmm_free_page(input_context_address);
        }
        if (ring != 0) {
            pmm_free_page(ring);
        }
        if (report != 0) {
            pmm_free_page(report);
        }
        release_keyboard_candidate();
        return 0;
    }
    controller.device_context = (uint8_t *)(uintptr_t)device_context;
    controller.input_context = (uint8_t *)(uintptr_t)input_context_address;
    controller.report = (uint8_t *)(uintptr_t)report;
    controller.device_context_array[slot] = device_context;
    ring_init(&controller.control_ring, ring);

    /* Address Device needs an input context and a transfer ring for EP0. */
    controller.input_context = (uint8_t *)(uintptr_t)input_context_address;
    diagnostics.enumeration_stage = 3;
    if (!set_address((uint8_t)speed, port)) {
        goto failed;
    }
    diagnostics.enumeration_stage = 4;
    if (!control_transfer(0x80, 6, 0x0100, 0, device_descriptor, 8) ||
        device_descriptor[0] < 8 || device_descriptor[1] != 1) {
        goto failed;
    }
    max_packet0 = device_descriptor[7];
    if (speed >= 4) {
        if (max_packet0 != 9) {
            goto failed;
        }
        max_packet0 = 1U << max_packet0;
    }
    if (!update_ep0_packet_size(max_packet0) ||
        !control_transfer(0x80, 6, 0x0100, 0,
                          device_descriptor, sizeof(device_descriptor))) {
        diagnostics.enumeration_stage = 5;
        goto failed;
    }
    diagnostics.enumeration_stage = 6;
    if (!control_transfer(0x80, 6, 0x0200, 0,
                          configuration, sizeof(configuration))) {
        goto failed;
    }
    total_length = configuration[2] | ((uint32_t)configuration[3] << 8);
    configuration_value = configuration[5];
    if (configuration[0] < 9 || configuration[1] != 2 ||
        total_length > sizeof(configuration) || total_length < 9) {
        diagnostics.enumeration_stage = 7;
        goto failed;
    }
    diagnostics.enumeration_stage = 8;
    for (descriptor_offset = 0; descriptor_offset + 2 <= total_length;) {
        uint8_t descriptor_length = configuration[descriptor_offset];
        uint8_t descriptor_type = configuration[descriptor_offset + 1];

        if (descriptor_length < 2 ||
            descriptor_offset + descriptor_length > total_length) {
            goto failed;
        }
        if (descriptor_type == 4 && descriptor_length >= 9) {
            interface_is_keyboard =
                configuration[descriptor_offset + 5] == 3 &&
                configuration[descriptor_offset + 6] == 1 &&
                configuration[descriptor_offset + 7] == 1;
            if (interface_is_keyboard) {
                interface_number = configuration[descriptor_offset + 2];
            }
        } else if (descriptor_type == 5 && descriptor_length >= 7 &&
                   interface_is_keyboard &&
                   (configuration[descriptor_offset + 2] & 0x80) != 0 &&
                   (configuration[descriptor_offset + 3] & 3) == 3) {
            endpoint_address = configuration[descriptor_offset + 2];
            endpoint_packet = configuration[descriptor_offset + 4] |
                ((uint32_t)configuration[descriptor_offset + 5] << 8);
            endpoint_interval = configuration[descriptor_offset + 6];
            break;
        }
        descriptor_offset += descriptor_length;
    }
    if (endpoint_address == 0 || endpoint_packet < 8 ||
        endpoint_packet > PAGE_SIZE || configuration_value == 0) {
        diagnostics.enumeration_stage = 9;
        goto failed;
    }
    diagnostics.enumeration_stage = 10;
    if (!control_transfer(0, 9, (uint16_t)configuration_value, 0, 0, 0) ||
        !control_transfer(0x21, 0x0B, 0, (uint16_t)interface_number, 0, 0)) {
        goto failed;
    }
    diagnostics.enumeration_stage = 11;
    ring = allocate_page();
    if (ring == 0) {
        goto failed;
    }
    ring_init(&controller.interrupt_ring, ring);
    if (speed <= 2) {
        uint32_t period_microframes = endpoint_interval * 8;
        uint32_t interval = 0;

        if (period_microframes == 0) {
            goto failed;
        }
        while (period_microframes > 1) {
            period_microframes >>= 1;
            ++interval;
        }
        endpoint_interval = interval;
    }
    if (endpoint_interval == 0 || endpoint_interval > 15) {
        diagnostics.enumeration_stage = 12;
        goto failed;
    }
    diagnostics.enumeration_stage = 13;
    if (!setup_interrupt_endpoint((uint8_t)endpoint_address,
                                  (uint16_t)endpoint_packet,
                                  (uint8_t)endpoint_interval)) {
        goto failed;
    }
    controller.keyboard_pending = 0;
    controller.caps_lock = 0;
    for (uint32_t i = 0; i < 6; ++i) {
        controller.previous_keys[i] = 0;
    }
    return 1;

failed:
    release_keyboard_candidate();
    return 0;
}

static int handoff_to_os(volatile uint32_t *capability, uint32_t offset) {
    volatile uint32_t *extended = capability + offset / 4;
    uint32_t current = offset;

    while (current != 0) {
        uint32_t header = read_reg(extended);
        uint32_t id = header & 0xFF;
        uint32_t next = (header >> 8) & 0xFF;

        if (id == 1) {
            write_reg(extended, header | (1U << 24));
            for (uint32_t i = 0; i < XHCI_WAIT_LIMIT; ++i) {
                header = read_reg(extended);
                if ((header & (1U << 16)) == 0) {
                    break;
                }
            }
            if ((header & (1U << 16)) != 0) {
                return 0;
            }
            uint32_t legacy = read_reg(extended + 1);
            write_reg(extended + 1, legacy & 0xFFFF0000U);
            break;
        }
        if (next == 0) {
            break;
        }
        current += next * 4;
        extended += next;
    }
    return 1;
}

static int initialize_controller(uint8_t bus, uint8_t device,
                                 uint8_t function) {
    uint32_t bar_low = pci_read32(bus, device, function, 0x10);
    uint32_t bar_high = 0;
    uint32_t bar_type;
    uint32_t command_register;
    uint64_t bar;
    volatile uint32_t *capability;
    uint32_t cap_length;
    uint32_t hcc_params;
    uint32_t scratchpad_low;
    uint32_t scratchpad_high;
    uint64_t dcbaa;
    uint64_t command_ring;
    uint64_t event_ring;
    uint64_t erst;
    struct xhci_erst_entry *erst_entry;
    uint32_t slots;
    uint32_t ports;
    uint32_t hcs_params1;
    uint32_t runtime_offset;
    uint32_t doorbell_offset;
    uint64_t scratchpad_array = 0;

    diagnostics.controller_stage = XHCI_CONTROLLER_STAGE_VALIDATE_BAR;
    bar_type = (bar_low >> 1) & 3;
    if ((bar_low & 1) != 0 || (bar_type != 0 && bar_type != 2)) {
        return 0;
    }
    if (bar_type == 2) {
        bar_high = pci_read32(bus, device, function, 0x14);
    }
    bar = ((uint64_t)bar_high << 32) | (bar_low & ~0xFULL);
    if (bar == 0) {
        return 0;
    }
    diagnostics.controller_stage = XHCI_CONTROLLER_STAGE_ENABLE_PCI;
    command_register = pci_read32(bus, device, function, 4);
    pci_write16(bus, device, function, 4,
                (uint16_t)(command_register | (1U << 1) | (1U << 2)));

    capability = (volatile uint32_t *)(uintptr_t)bar;
    diagnostics.controller_stage =
        XHCI_CONTROLLER_STAGE_VALIDATE_CAPABILITIES;
    cap_length = *(volatile uint8_t *)(uintptr_t)bar;
    hcc_params = read_reg(capability + 4);
    hcs_params1 = read_reg(capability + 1);
    slots = hcs_params1 & 0xFF;
    ports = hcs_params1 >> 24;

    /* Expose raw capability/register values for post-mortem diagnostics. */
    diagnostics.cap_length = cap_length;
    diagnostics.hcc_params = hcc_params;
    diagnostics.hcs_params1 = hcs_params1;
    diagnostics.mmio_bar = bar;

    if (cap_length < 0x20 || slots == 0 || ports == 0) {
        return 0;
    }
    controller.operational = (volatile uint32_t *)(uintptr_t)(bar + cap_length);
    doorbell_offset = read_reg(capability + 5) & ~3U;
    runtime_offset = read_reg(capability + 6) & ~0x1FU;

    diagnostics.doorbell_offset = doorbell_offset;
    diagnostics.runtime_offset = runtime_offset;

    controller.doorbells =
        (volatile uint32_t *)(uintptr_t)(bar + doorbell_offset);
    controller.runtime_base =
        (volatile uint32_t *)(uintptr_t)(bar + runtime_offset);
    controller.runtime = controller.runtime_base + 0x20 / sizeof(uint32_t);
    controller.max_ports = ports;
    controller.max_slots = slots;
    controller.context_size = (hcc_params & (1U << 2)) != 0 ? 64 : 32;
    controller.port_power_control = (hcc_params >> 3) & 1U;
    controller.supports_64bit_dma = hcc_params & 1U;

    diagnostics.controller_stage = XHCI_CONTROLLER_STAGE_FIRMWARE_HANDOFF;
    if (!handoff_to_os(capability, ((hcc_params >> 16) & 0xFFFF) * 4)) {
        return 0;
    }

    diagnostics.controller_stage = XHCI_CONTROLLER_STAGE_RESET;
    if ((read_reg(controller.operational) & 1) != 0) {
        write_reg(controller.operational, read_reg(controller.operational) & ~1U);
        if (!wait_reg(controller.operational + 1, 1, 1)) {
            return 0;
        }
    }
    write_reg(controller.operational, read_reg(controller.operational) | (1U << 1));
    if (!wait_reg(controller.operational, 1U << 1, 0) ||
        !wait_reg(controller.operational + 1, 1U << 11, 0)) {
        return 0;
    }
    controller.max_slots = slots < 32 ? slots : 32;

    diagnostics.controller_stage = XHCI_CONTROLLER_STAGE_ALLOCATE_RINGS;
    dcbaa = allocate_page();
    command_ring = allocate_page();
    event_ring = allocate_page();
    erst = allocate_page();
    if (dcbaa == 0 || command_ring == 0 || event_ring == 0 || erst == 0) {
        return 0;
    }
    controller.device_context_array = (uint64_t *)(uintptr_t)dcbaa;
    controller.dcbaa_physical = dcbaa;
    controller.event_ring = (struct xhci_trb *)(uintptr_t)event_ring;
    controller.event_ring_physical = event_ring;
    controller.erst_physical = erst;
    controller.event_index = 0;
    controller.event_cycle = 1;
    ring_init(&controller.command_ring, command_ring);

    diagnostics.controller_stage =
        XHCI_CONTROLLER_STAGE_ALLOCATE_SCRATCHPADS;
    scratchpad_high = (read_reg(capability + 2) >> 27) & 0x1F;
    scratchpad_low = (read_reg(capability + 2) >> 21) & 0x1F;
    controller.scratchpad_count = (scratchpad_high << 5) | scratchpad_low;
    if (controller.scratchpad_count != 0) {
        if (controller.scratchpad_count > 512) {
            return 0;
        }
        scratchpad_array = allocate_page();
        if (scratchpad_array == 0) {
            return 0;
        }
        controller.scratchpad_array_physical = scratchpad_array;
        controller.device_context_array[0] = scratchpad_array;
        for (uint32_t i = 0; i < controller.scratchpad_count; ++i) {
            uint64_t scratchpad = allocate_page();
            if (scratchpad == 0) {
                return 0;
            }
            ((uint64_t *)(uintptr_t)scratchpad_array)[i] = scratchpad;
        }
    }
    erst_entry = (struct xhci_erst_entry *)(uintptr_t)erst;
    erst_entry->ring_segment_base = event_ring;
    erst_entry->ring_segment_size = XHCI_EVENT_TRBS;
    controller.runtime[0x08 / 4] = 1;
    controller.runtime[0x10 / 4] = (uint32_t)erst;
    controller.runtime[0x14 / 4] = (uint32_t)(erst >> 32);
    controller.runtime[0x18 / 4] = (uint32_t)event_ring;
    controller.runtime[0x1C / 4] = (uint32_t)(event_ring >> 32);
    write_reg(controller.operational + 0x30 / 4, (uint32_t)dcbaa);
    write_reg(controller.operational + 0x34 / 4, (uint32_t)(dcbaa >> 32));
    write_reg(controller.operational + 0x18 / 4, (uint32_t)command_ring | 1U);
    write_reg(controller.operational + 0x1C / 4, (uint32_t)(command_ring >> 32));
    write_reg(controller.operational + 0x38 / 4, controller.max_slots);
    diagnostics.controller_stage = XHCI_CONTROLLER_STAGE_START;
    write_reg(controller.operational, read_reg(controller.operational) | 1U);
    if (!wait_reg(controller.operational + 1, 1, 0)) {
        return 0;
    }
    diagnostics.controller_stage = XHCI_CONTROLLER_STAGE_RUNNING;
    return 1;
}

static int find_controller(void) {
    while (pci_scan_index < 256U * 32U * 8U) {
        uint32_t index = pci_scan_index++;
        uint8_t bus = (uint8_t)(index >> 8);
        uint8_t device = (uint8_t)((index >> 3) & 0x1F);
        uint8_t function = (uint8_t)(index & 7);
        uint32_t identity = pci_read32(bus, device, function, 0);
        uint32_t class_code;

        if ((identity & 0xFFFF) == 0xFFFF) {
            if (function == 0) {
                pci_scan_index = (pci_scan_index + 7U) & ~7U;
            }
            continue;
        }
        class_code = pci_read32(bus, device, function, 8);
        if ((class_code >> 8) == 0x0C0330) {
            pci_xhci_found = 1;
            diagnostics.pci_location = ((uint32_t)bus << 16) |
                                       ((uint32_t)device << 8) | function;
            diagnostics.pci_identity = identity;
            for (uint32_t i = 0; i < sizeof(controller); ++i) {
                ((uint8_t *)&controller)[i] = 0;
            }
            if (initialize_controller(bus, device, function)) {
                return 1;
            }
        }
    }
    return pci_xhci_found ? -1 : 0;
}

static enum xhci_init_status scan_controller_ports(void) {
    int connected = 0;
    int reset_succeeded = 0;

    diagnostics.port_count = controller.max_ports;
    if (controller.port_power_control) {
        for (uint32_t port = 1; port <= controller.max_ports; ++port) {
            uint32_t port_status = read_portsc(port);
            if ((port_status & (1U << 9)) == 0) {
                write_portsc(port, 1U << 9);
            }
        }
    }
    if (!wait_microframes(XHCI_PORT_DEBOUNCE_MICROFRAMES)) {
        for (uint32_t port = 1; port <= controller.max_ports; ++port) {
            set_port_stage(port, XHCI_PORT_STAGE_POWER_TIMEOUT);
            diagnostics.port_status[port - 1] = read_portsc(port);
        }
        return XHCI_INIT_FAILED;
    }
    for (uint32_t port = 1; port <= controller.max_ports; ++port) {
        uint32_t speed;
        uint32_t port_status = read_portsc(port);
        int port_connected = (port_status & 1U) != 0;

        diagnostics.port_status[port - 1] = port_status;
        diagnostics.port_stages[port - 1] =
            XHCI_PORT_STAGE_NO_CONNECTION;
        if (diagnostics.connected_ports == 0) {
            diagnostics.last_port = port;
            diagnostics.last_port_status = port_status;
        }
        if (port_connected) {
            connected = 1;
            ++diagnostics.connected_ports;
            diagnostics.last_port = port;
            diagnostics.last_port_status = port_status;
            diagnostics.last_speed = (port_status >> 10) & 0x0F;
            diagnostics.port_stage = XHCI_PORT_STAGE_DEBOUNCE;
            diagnostics.port_stages[port - 1] =
                XHCI_PORT_STAGE_DEBOUNCE;
            diagnostics.enumeration_stage = 0;
            diagnostics.last_command_type = 0;
            diagnostics.last_completion_code = 0;
        } else {
            continue;
        }
        if (!reset_port(port, &speed)) {
            if (port_connected) {
                diagnostics.last_port_status = read_portsc(port);
                diagnostics.port_status[port - 1] =
                    diagnostics.last_port_status;
                diagnostics.last_speed = (diagnostics.last_port_status >> 10) & 0x0F;
            }
            continue;
        }
        reset_succeeded = 1;
        ++diagnostics.reset_ports;
        diagnostics.last_speed = speed;
        diagnostics.last_port_status = read_portsc(port);
        diagnostics.port_status[port - 1] = diagnostics.last_port_status;
        diagnostics.enumeration_stage = 0;
        if (usb_find_keyboard(port, speed)) {
            controller.keyboard_port = port;
            return 1;
        }
        diagnostics.port_enumeration_stages[port - 1] =
            diagnostics.enumeration_stage;
        diagnostics.port_command_types[port - 1] =
            diagnostics.last_command_type;
        diagnostics.port_completion_codes[port - 1] =
            diagnostics.last_completion_code;
    }
    if (!connected) {
        set_port_stage(0, XHCI_PORT_STAGE_NO_CONNECTION);
        return XHCI_INIT_NO_KEYBOARD;
    }
    return reset_succeeded ? XHCI_INIT_NO_KEYBOARD : XHCI_INIT_FAILED;
}

static void copy_diagnostics(struct xhci_diagnostics *destination,
                             const struct xhci_diagnostics *source) {
    const uint32_t *source_words =
        (const uint32_t *)(const void *)source;
    uint32_t *destination_words = (uint32_t *)(void *)destination;

    for (uint32_t i = 0; i < sizeof(*destination) / sizeof(uint32_t); ++i) {
        destination_words[i] = source_words[i];
    }
}

static int stop_controller(void) {
    if (controller.operational == 0) {
        return 1;
    }
    write_reg(controller.operational,
              read_reg(controller.operational) & ~1U);
    if (!wait_reg(controller.operational + 1, 1, 1)) {
        return 0;
    }
    if (controller.scratchpad_array_physical != 0) {
        uint64_t *scratchpads =
            (uint64_t *)(uintptr_t)controller.scratchpad_array_physical;

        for (uint32_t i = 0; i < controller.scratchpad_count; ++i) {
            if (scratchpads[i] != 0) {
                pmm_free_page(scratchpads[i]);
            }
        }
        pmm_free_page(controller.scratchpad_array_physical);
    }
    if (controller.dcbaa_physical != 0) {
        pmm_free_page(controller.dcbaa_physical);
    }
    if (controller.command_ring.physical != 0) {
        pmm_free_page(controller.command_ring.physical);
    }
    if (controller.event_ring_physical != 0) {
        pmm_free_page(controller.event_ring_physical);
    }
    if (controller.erst_physical != 0) {
        pmm_free_page(controller.erst_physical);
    }
    return 1;
}

enum xhci_init_status xhci_init(void) {
    int status;
    int initialized_controller = 0;
    uint32_t controllers_scanned = 0;
    uint32_t controllers_with_connected_ports = 0;
    int port_initialization_failed = 0;
    int have_best_diagnostics = 0;
    struct xhci_diagnostics best_diagnostics;

    for (uint32_t i = 0; i < sizeof(controller); ++i) {
        ((uint8_t *)&controller)[i] = 0;
    }
    for (uint32_t i = 0; i < sizeof(diagnostics); ++i) {
        ((uint8_t *)&diagnostics)[i] = 0;
    }
    pci_scan_index = 0;
    pci_xhci_found = 0;
    controller_diagnostic_count = 0;

    while ((status = find_controller()) > 0) {
        uint32_t pci_location = diagnostics.pci_location;
        uint32_t pci_identity = diagnostics.pci_identity;
        uint32_t cap_length = diagnostics.cap_length;
        uint32_t hcc_params = diagnostics.hcc_params;
        uint32_t hcs_params1 = diagnostics.hcs_params1;
        uint64_t mmio_bar = diagnostics.mmio_bar;
        uint32_t doorbell_offset = diagnostics.doorbell_offset;
        uint32_t runtime_offset = diagnostics.runtime_offset;
        enum xhci_init_status port_status;

        initialized_controller = 1;
        ++controllers_scanned;
        for (uint32_t i = 0; i < sizeof(diagnostics); ++i) {
            ((uint8_t *)&diagnostics)[i] = 0;
        }
        diagnostics.pci_location = pci_location;
        diagnostics.pci_identity = pci_identity;
        diagnostics.controller_stage = XHCI_CONTROLLER_STAGE_RUNNING;
        diagnostics.cap_length = cap_length;
        diagnostics.hcc_params = hcc_params;
        diagnostics.hcs_params1 = hcs_params1;
        diagnostics.mmio_bar = mmio_bar;
        diagnostics.doorbell_offset = doorbell_offset;
        diagnostics.runtime_offset = runtime_offset;
        port_status = scan_controller_ports();
        if (port_status == XHCI_INIT_READY) {
            return port_status;
        }
        if (port_status == XHCI_INIT_FAILED) {
            port_initialization_failed = 1;
        }
        if (controller_diagnostic_count <
            XHCI_MAX_DIAGNOSTIC_CONTROLLERS) {
            copy_diagnostics(
                &controller_diagnostics[controller_diagnostic_count],
                &diagnostics);
            ++controller_diagnostic_count;
        }
        if (diagnostics.connected_ports != 0) {
            ++controllers_with_connected_ports;
            copy_diagnostics(&best_diagnostics, &diagnostics);
            have_best_diagnostics = 1;
        } else if (!have_best_diagnostics) {
            copy_diagnostics(&best_diagnostics, &diagnostics);
            have_best_diagnostics = 1;
        }
        if (!stop_controller()) {
            return XHCI_INIT_FAILED;
        }
        for (uint32_t i = 0; i < sizeof(controller); ++i) {
            ((uint8_t *)&controller)[i] = 0;
        }
    }
    if (initialized_controller) {
        if (have_best_diagnostics) {
            copy_diagnostics(&diagnostics, &best_diagnostics);
        }
        diagnostics.controllers_scanned = controllers_scanned;
        diagnostics.controllers_with_connected_ports =
            controllers_with_connected_ports;
        return port_initialization_failed ? XHCI_INIT_FAILED :
                                            XHCI_INIT_NO_KEYBOARD;
    }
    if (status < 0) {
        return XHCI_INIT_FAILED;
    }
    return XHCI_INIT_NO_CONTROLLER;
}

void xhci_get_diagnostics(struct xhci_diagnostics *result) {
    if (result == 0) {
        return;
    }
    *result = diagnostics;
}

uint32_t xhci_get_controller_diagnostic_count(void) {
    return controller_diagnostic_count;
}

int xhci_get_controller_diagnostics(
    uint32_t index, struct xhci_diagnostics *result) {
    if (result == 0 || index >= controller_diagnostic_count) {
        return 0;
    }
    copy_diagnostics(result, &controller_diagnostics[index]);
    result->controllers_scanned = diagnostics.controllers_scanned;
    result->controllers_with_connected_ports =
        diagnostics.controllers_with_connected_ports;
    return 1;
}

int xhci_keyboard_connected(void) {
    if (controller.slot_id == 0 || controller.keyboard_port == 0 ||
        controller.keyboard_port > controller.max_ports) {
        return 0;
    }
    return (read_portsc(controller.keyboard_port) & 1U) != 0;
}

static int keycode_to_char(uint32_t key, uint8_t modifiers, uint8_t caps) {
    int shift = (modifiers & ((1U << 1) | (1U << 5))) != 0;

    if (key >= 4 && key <= 29) {
        char character = (char)('a' + key - 4);
        if (shift != (caps != 0)) {
            character = (char)(character - 'a' + 'A');
        }
        return character;
    }
    if (key >= 30 && key <= 38) {
        static const char plain[] = "123456789";
        static const char shifted[] = "!@#$%^&*(";
        return shift ? shifted[key - 30] : plain[key - 30];
    }
    switch (key) {
    case 0x27: return shift ? ')' : '0';
    case 0x28: return '\r';
    case 0x29: return 0x1B;
    case 0x2A: return '\b';
    case 0x2B: return '\t';
    case 0x2C: return ' ';
    case 0x2D: return shift ? '_' : '-';
    case 0x2E: return shift ? '+' : '=';
    case 0x2F: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    default: return -1;
    }
}

int xhci_read_char(void) {
    struct xhci_trb trb;
    uint32_t completion = 0;
    uint32_t residual = 0;
    int character = -1;

    if (controller.slot_id == 0) {
        return -1;
    }
    if (!controller.keyboard_pending) {
        trb.parameter = (uint64_t)(uintptr_t)controller.report;
        trb.status = controller.report_length;
        trb.control = (1U << 10) | TRB_INTERRUPT;
        ring_push(&controller.interrupt_ring, &trb);
        ring_doorbell(controller.slot_id, controller.endpoint_id);
        controller.keyboard_pending = 1;
    }
    if (!poll_transfer(controller.endpoint_id, &completion, &residual)) {
        return -1;
    }
    controller.keyboard_pending = 0;
    if (completion != 1 || residual > controller.report_length ||
        controller.report_length - residual < 8) {
        return -1;
    }
    {
        uint8_t modifiers = controller.report[0];
        uint32_t keys[6];
        int caps_was_down = 0;
        int caps_is_down = 0;

        for (uint32_t i = 0; i < 6; ++i) {
            keys[i] = controller.report[2 + i];
            if (keys[i] == 1 || keys[i] == 2 || keys[i] == 3) {
                return -1;
            }
            if (controller.previous_keys[i] == 0x39) {
                caps_was_down = 1;
            }
            if (keys[i] == 0x39) {
                caps_is_down = 1;
            }
        }
        if (caps_is_down && !caps_was_down) {
            controller.caps_lock ^= 1;
        }
        for (uint32_t i = 0; i < 6; ++i) {
            int was_down = 0;

            if (keys[i] == 0) {
                continue;
            }
            for (uint32_t j = 0; j < 6; ++j) {
                if (controller.previous_keys[j] == keys[i]) {
                    was_down = 1;
                    break;
                }
            }
            if (!was_down && keys[i] != 0x39) {
                character = keycode_to_char(keys[i], modifiers,
                                            controller.caps_lock);
                if (character >= 0) {
                    break;
                }
            }
        }
        for (uint32_t i = 0; i < 6; ++i) {
            controller.previous_keys[i] = keys[i];
        }
    }
    return character;
}
