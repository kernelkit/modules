AMD/Xilinx PG210 Timer Syncer Driver
====================================

[PHC][ptp] driver for the PTP 1588 Timer Syncer described in
[AMD/Xilinx PG210][pg210].  It registers one PTP clock, which shows up
as `/dev/ptpN`, and drives the system timer that feeds time of day to
the Ethernet ports.


Binding
-------

The driver is a platform driver with no ID table.  The device must be
created by its parent and bound by `driver_override`.  `pasw-saline`
works this way: it carves the timer's registers out of its PCI BAR,
creates `<pci-dev>.ptp`, and loads `pg210-ptp` itself, since module
autoloading ignores the override.

The device needs:

- One memory resource covering the timer's registers
- A clock named `ts_clk`, the timestamp reference clock

The nominal period of the system timer comes from the `ts_clk` rate.


Ports
-----

At probe the driver scans all 16 port slots.  A port is used if both
its Tx and Rx clocks are connected and run at the same rate.  Ports
with asymmetric clocks are skipped with a warning.  The driver fails
to probe if no port is usable.


In-Kernel API
-------------

`pg210_ptp_gettime()` lets other drivers read the timer without
going through `/dev/ptpN`.  `pasw-saline` uses it for scheduled
traffic and resolves it with `symbol_get()`, so `pg210-ptp` is not a
hard dependency.

[ptp]:   https://docs.kernel.org/driver-api/ptp.html
[pg210]: https://docs.amd.com/r/en-US/pg210-25g-ethernet
