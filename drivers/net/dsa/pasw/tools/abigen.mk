pasw-src := $(M)/drivers/net/dsa/pasw
abigen-cmd := $(pasw-src)/tools/abigen.py

quiet_cmd_abispec = ABISPEC $@
      cmd_abispec = $(abigen-cmd) spec $< >$@

quiet_cmd_abigen = ABIGEN  $@
      cmd_abigen = $(abigen-cmd) gen $(2) $(filter %.yml,$+) >$@

define abispec-rules

abispec_$(1) ?= $(pasw-src)/pasw-abi.yml

always-y += $(1)
$$(addprefix $$(obj)/,$$(abideps_$(1))): $$(obj)/$(1)
$$(obj)/$(1): $$(abispec_$(1)) $(abigen-cmd) FORCE
	$$(call if_changed,abispec)

endef
$(eval $(foreach spec,$(abispec-y),$(call abispec-rules,$(spec))))

define abigen-rules

abispec_$(1) ?= $(pasw-src)/pasw-abi.yml
abiname_$(1) ?= $(subst -abi.c,,$(1))
abiregs_$(1) ?= $$(src)/$$(abiname_$(1))-regs.yml
abiconf_$(1) ?= $$(src)/$$(abiname_$(1))-conf.yml

always-y += $(1)
$$(addprefix $$(obj)/,$$(abideps_$(1))): $$(obj)/$(1)
$$(obj)/$(1): $$(abispec_$(1)) $$(abiregs_$(1)) $$(abiconf_$(1)) $(abigen-cmd) FORCE
	$$(call if_changed,abigen,$$(abiname_$(1)))

endef
$(eval $(foreach abi,$(abigen-y),$(call abigen-rules,$(abi))))
