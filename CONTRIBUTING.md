Contributing
============

Patches are welcome as GitHub pull requests.


Code
----

- Follow the kernel [coding style][style] and run `checkpatch.pl` on
  your changes
- Every source file starts with an SPDX license identifier, normally
  `GPL-2.0-only`
- New modules follow the layout in the README's "Adding a Module"
  section, and get a `README.md` next to the code


Commits
-------

- One logical change per commit, and each commit must build
- Prefix the summary line with the module, e.g., `pasw: fix VLAN
  flush on port leave`
- Sign off every commit (`git commit -s`) to certify the [Developer
  Certificate of Origin][dco]

[style]: https://docs.kernel.org/process/coding-style.html
[dco]:   https://developercertificate.org/
