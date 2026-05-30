#ifndef ACPI_H
#define ACPI_H

// Minimal ACPI table parser — just enough to find the FADT reset register
// and perform a clean reboot. No AML, no DSDT, no namespace.

// Call once during kernel init (after VMM/heap are up).
// Returns 1 on success, 0 if ACPI tables not found.
int acpi_init(void);

// Write the FADT reset register to reboot. Falls back to KBC if ACPI
// reset register is not present. Never returns.
void acpi_reboot(void);

// Write ACPI S5 sleep to power off. Falls back to legacy ports.
void acpi_poweroff(void);

#endif