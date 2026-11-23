
obj-m := pasw-saline.o

pasw-saline-objs := saline.o

abigen-y := saline-abi.c
abideps_saline-abi.c := $(pasw-saline-objs)

include $(src)/../tools/abigen.mk
