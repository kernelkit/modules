Kernelkit External Kernel Modules
=================================

Linux kernel drivers maintained by [Kernelkit][kernelkit], built as
[external modules][kbuild-ext].  The primary consumer is [Infix][infix],
but nothing here depends on it.


Modules
-------

| Option                  | Module(s)                     | Description                                                       |
|-------------------------|-------------------------------|-------------------------------------------------------------------|
| `MOD_NET_DSA_PASW`      | `pasw`, `tag_pasw`, `pasw-*`  | [DSA][dsa] driver for [Packet Architects'][packetarc] FlexSwitch, see [its README][pasw] |
| `MOD_NET_ETH_XDMA_NET`  | `xdma-net`                    | Net device on the [AMD/Xilinx XDMA][pg195] (PG195), see [its README][xdma] |
| `MOD_PTP_PG210_PTP`     | `pg210-ptp`                   | [PHC][ptp] driver for the Timer Syncer in [AMD/Xilinx PG210][pg210], see [its README][pg210-ptp] |


Requirements
------------

- A configured and built kernel tree.  Some modules use kernel-internal
  headers, so your distribution's `linux-headers` package is not enough
- Python 3 with [PyYAML][pyyaml], for `pasw`, which generates parts of
  its source at build time
- `kconfig-mconf` from `kconfig-frontends`, only for `make menuconfig`


Building
--------

Select modules with `make menuconfig`, enable all of them with `make
allmodconfig`, or set the options directly:

```sh
make KDIR=/path/to/linux MOD_NET_DSA_PASW=m MOD_PTP_PG210_PTP=m
make KDIR=/path/to/linux INSTALL_MOD_PATH=/path/to/rootfs install
```

Nothing is built unless at least one option is set to `m`.

| Variable | Default                               | Purpose                                |
|----------|---------------------------------------|----------------------------------------|
| `KDIR`   | `/lib/modules/$(uname -r)/build`      | Configured and built kernel tree       |
| `MO`     | `.`                                   | Directory holding the module `.config` |

Cross-compiling works as for any external module: pass `ARCH` and
`CROSS_COMPILE`.  `make install` runs `modules_install`, which honors
`INSTALL_MOD_PATH`.


Adding a Module
---------------

Place it where it would live in the kernel tree, e.g.,
`drivers/net/phy/foo/`, with a `Kconfig` and a `Makefile` keyed on
`MOD_<SYMBOL>`, and hook both into the parent directory's files.
Symbols are tristate and `depends on m`.  See [CONTRIBUTING](CONTRIBUTING.md)
for how to submit changes.


License
-------

[GPL-2.0](COPYING)

[kernelkit]:  https://github.com/kernelkit
[kbuild-ext]: https://docs.kernel.org/kbuild/modules.html
[infix]:      https://github.com/kernelkit/infix
[dsa]:        https://docs.kernel.org/networking/dsa/dsa.html
[packetarc]:  https://www.packetarc.com/
[pg195]:      https://docs.amd.com/r/en-US/pg195-pcie-dma
[pg210]:      https://docs.amd.com/r/en-US/pg210-25g-ethernet
[ptp]:        https://docs.kernel.org/driver-api/ptp.html
[pyyaml]:     https://pyyaml.org/
[pasw]:       drivers/net/dsa/pasw/README.md
[xdma]:       drivers/net/ethernet/xdma-net/README.md
[pg210-ptp]:  drivers/ptp/pg210-ptp/README.md
