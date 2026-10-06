#ifndef ASTRAOS_XHCI_H
#define ASTRAOS_XHCI_H

#include <stdint.h>

#define XHCI_MAX_PORTS 255U
#define XHCI_MAX_DIAGNOSTIC_CONTROLLERS 8U

enum xhci_init_status {
    XHCI_INIT_FAILED = -1,
    XHCI_INIT_NO_KEYBOARD = -2,
    XHCI_INIT_NO_CONTROLLER = 0,
    XHCI_INIT_READY = 1
};

enum xhci_controller_stage {
    XHCI_CONTROLLER_STAGE_NONE = 0,
    XHCI_CONTROLLER_STAGE_VALIDATE_BAR = 1,
    XHCI_CONTROLLER_STAGE_ENABLE_PCI = 2,
    XHCI_CONTROLLER_STAGE_VALIDATE_CAPABILITIES = 3,
    XHCI_CONTROLLER_STAGE_FIRMWARE_HANDOFF = 4,
    XHCI_CONTROLLER_STAGE_RESET = 5,
    XHCI_CONTROLLER_STAGE_ALLOCATE_RINGS = 6,
    XHCI_CONTROLLER_STAGE_ALLOCATE_SCRATCHPADS = 7,
    XHCI_CONTROLLER_STAGE_START = 8,
    XHCI_CONTROLLER_STAGE_RUNNING = 9
};

enum xhci_port_stage {
    XHCI_PORT_STAGE_NONE = 0,
    XHCI_PORT_STAGE_NO_CONNECTION = 1,
    XHCI_PORT_STAGE_DEBOUNCE = 2,
    XHCI_PORT_STAGE_CONNECTION_LOST = 3,
    XHCI_PORT_STAGE_RESET = 4,
    XHCI_PORT_STAGE_RESET_TIMEOUT = 5,
    XHCI_PORT_STAGE_NOT_ENABLED = 6,
    XHCI_PORT_STAGE_READY = 7,
    XHCI_PORT_STAGE_POWER_TIMEOUT = 8
};

struct xhci_diagnostics {
    uint32_t pci_location;
    uint32_t pci_identity;
    uint32_t controllers_scanned;
    uint32_t controllers_with_connected_ports;
    uint32_t controller_stage;
    uint32_t port_count;
    uint32_t connected_ports;
    uint32_t reset_ports;
    uint32_t last_port;
    uint32_t last_port_status;
    uint32_t last_speed;
    uint32_t port_stage;
    uint32_t enumeration_stage;
    uint32_t last_command_type;
    uint32_t last_completion_code;
    /* Raw controller capability/register values for diagnostics */
    uint32_t cap_length;
    uint32_t hcc_params;
    uint32_t hcs_params1;
    uint64_t mmio_bar;
    uint32_t doorbell_offset;
    uint32_t runtime_offset;
    uint32_t port_status[XHCI_MAX_PORTS];
    uint32_t port_stages[XHCI_MAX_PORTS];
    uint32_t port_enumeration_stages[XHCI_MAX_PORTS];
    uint32_t port_command_types[XHCI_MAX_PORTS];
    uint32_t port_completion_codes[XHCI_MAX_PORTS];
};

enum xhci_init_status xhci_init(void);
void xhci_get_diagnostics(struct xhci_diagnostics *diagnostics);
uint32_t xhci_get_controller_diagnostic_count(void);
int xhci_get_controller_diagnostics(
    uint32_t index, struct xhci_diagnostics *diagnostics);
int xhci_keyboard_connected(void);
int xhci_read_char(void);

#endif
