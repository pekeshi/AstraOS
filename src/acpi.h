#ifndef ASTRAOS_ACPI_H
#define ASTRAOS_ACPI_H

enum acpi_shutdown_result {
    ACPI_SHUTDOWN_NO_TABLES,
    ACPI_SHUTDOWN_INVALID_TABLES,
    ACPI_SHUTDOWN_NO_S5,
    ACPI_SHUTDOWN_UNSUPPORTED,
    ACPI_SHUTDOWN_DID_NOT_POWER_OFF
};

enum acpi_shutdown_result acpi_request_shutdown(const void *rsdp_address);

#endif
