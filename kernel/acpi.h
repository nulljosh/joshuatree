#ifndef ACPI_H
#define ACPI_H
/* 13.25.0: turn the PC off through ACPI (issue #1018), kernel/acpi.c. Returns only if the machine is still on:
   -1 no RSDP, -2 no FADT, -3 no \_S5_ in the DSDT, -4 the write did not power off. */
int acpi_poweroff(void);
#endif
