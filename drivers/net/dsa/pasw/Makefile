ccflags-y := -I$(src)/include

# Building a tagger as an external module requires access to the
# internal headers
CFLAGS_tag_pasw.o += -I$(srctree)/net/dsa

pasw-objs += pasw-abi.o
pasw-objs += pasw-clk.o
pasw-objs += pasw-core.o
pasw-objs += pasw-dcb.o
pasw-objs += pasw-debugfs.o
pasw-objs += pasw-l2.o
pasw-objs += pasw-tc.o
pasw-objs += pasw-vlan.o

obj-m := pasw.o tag_pasw.o
obj-m += dev-saline/

abispec-y += pasw-abi-spec.h
abideps_pasw-abi-spec.h := $(pasw-objs) $(obj-m)
include $(src)/tools/abigen.mk
