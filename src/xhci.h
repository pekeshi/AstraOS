#ifndef ASTRAOS_XHCI_H
#define ASTRAOS_XHCI_H

enum xhci_init_status {
    XHCI_INIT_FAILED = -1,
    XHCI_INIT_NO_KEYBOARD = -2,
    XHCI_INIT_NO_CONTROLLER = 0,
    XHCI_INIT_READY = 1
};

enum xhci_init_status xhci_init(void);
int xhci_read_char(void);

#endif
