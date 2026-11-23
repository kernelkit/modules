External Linux Kernel Modules by Kernelkit
==========================================

This repository contains a set of Linux kernel modules that can be
built as external modules. The primary consumer is [Infix][infix], but
this repo does not presuppose that in any way.

Some modules depend on kernel-internal headers, and thus require the
full kernel sources to build. I.e., your distribution's
`linux-headers` package is not sufficient.


Modules
-------

| Option                  | Module(s)                     | Description                          |
|-------------------------|-------------------------------|--------------------------------------|
| `MOD_NET_DSA_PASW`      | `pasw`, `tag_pasw`, `pasw-*`  | DSA driver for Packet Architects' FlexSwitch, see [its README][pasw] |
| `MOD_NET_ETH_XDMA_NET`  | `xdma-net`                    | Net device on AMD/Xilinx XDMA, see [its README][xdma] |
| `MOD_PTP_PG210_PTP`     | `pg210-ptp`                   | PHC driver for the AMD/Xilinx PG210 Timer Syncer |

`pasw` generates parts of its source at build time, which requires
Python 3 with PyYAML on the build host.


Building
--------

Select modules with `make menuconfig` (needs `kconfig-mconf` from
`kconfig-frontends`), or set the options directly:

    make KDIR=/path/to/linux MOD_NET_DSA_PASW=m

Nothing is built unless at least one option is set to `m`.

| Variable | Default                               | Purpose                               |
|----------|---------------------------------------|---------------------------------------|
| `KDIR`   | `/lib/modules/$(uname -r)/build`      | Configured and built kernel tree      |
| `MO`     | `.`                                   | Directory holding the module `.config`|

Cross-compiling works as for any external module: pass `ARCH` and
`CROSS_COMPILE`. `make install` runs `modules_install`, which honors
`INSTALL_MOD_PATH`.


Adding a Module
---------------

Place it where it would live in the kernel tree, e.g.
`drivers/net/phy/foo/`, with a `Kconfig` and a `Makefile` keyed on
`MOD_<SYMBOL>`, and hook both into the parent directory's files.
Symbols are tristate and `depends on m`.


License
-------

GPL-2.0, see `COPYING`.

[infix]: https://github.com/kernelkit/infix
[pasw]: drivers/net/dsa/pasw/README.md
[xdma]: drivers/net/ethernet/xdma-net/README.md
