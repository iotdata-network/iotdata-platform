
# Makefile — iotdata-platform.
#
# This repo builds nothing of its own: it is headers, app bodies and host test
# suites that other repos compile. So the only target that does real work is
# `format`, which runs clang-format over everything in here that clang-format
# owns. More targets can land beside it later (the test suites in tests/ are the
# obvious next one) without changing the shape of this file.
#
# Paths resolve from THIS file's own location, the same idiom make/config.mk uses
# on itself, so `make -f .../iotdata-platform/Makefile format` works from any
# directory, and so an overridden IOTDATA_SRC can never aim `format` at a
# different tree than the one this Makefile sits in.
#
# NB: keep comments on their own lines — a trailing "# ..." after a value would
# leave the spaces before the # in the variable.

_HERE := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

include $(_HERE)/make/config.mk

##

# Everything clang-format owns here: the node headers, the device drivers beside
# them, the app bodies, and the host test suites. Wildcards rather than a hand
# list — a new header is formatted the day it appears, with nothing to remember.
FORMAT_SOURCES := \
    $(wildcard $(_HERE)/include/*.h) \
    $(wildcard $(_HERE)/include/device/*.h) \
    $(wildcard $(_HERE)/apps/*.c) \
    $(wildcard $(_HERE)/tests/unit/*.c)

##

# help, not format: a bare `make` here must never rewrite the tree. Reformatting
# is a thing you ask for, especially with work in progress in the working copy.
.DEFAULT_GOAL := help

.PHONY: format format-check config help

format: ## rewrite every source clang-format owns, in place
	$(FORMAT) -i $(FORMAT_SOURCES)

format-check: ## report what `format` would change, rewriting nothing (exits 1 if any)
	@$(FORMAT) --dry-run --Werror $(FORMAT_SOURCES)

config: ## show the resolved paths and file counts
	@echo "IOTDATA_APEX         = $(IOTDATA_APEX)"
	@echo "IOTDATA_SRC          = $(IOTDATA_SRC)"
	@echo "IOTDATA_SRC_PLATFORM = $(IOTDATA_SRC_PLATFORM)"
	@echo "IOTDATA_HOST         = $(IOTDATA_HOST)"
	@echo "IOTDATA_DEFS         = $(if $(IOTDATA_DEFS),$(IOTDATA_DEFS),(none for this host))"
	@echo "FORMAT               = $(FORMAT)"
	@echo "FORMAT_SOURCES       = $(words $(FORMAT_SOURCES)) files"
	@echo "  include/*.h         $(words $(wildcard $(_HERE)/include/*.h))"
	@echo "  include/device/*.h  $(words $(wildcard $(_HERE)/include/device/*.h))"
	@echo "  apps/*.c            $(words $(wildcard $(_HERE)/apps/*.c))"
	@echo "  tests/unit/*.c      $(words $(wildcard $(_HERE)/tests/unit/*.c))"

help: ## this list
	@echo "iotdata-platform — headers, app bodies and host tests; nothing to build"
	@echo
	@awk 'BEGIN{FS=":.*## "} /^[a-zA-Z_-]+:.*## /{printf "  make %-13s %s\n",$$1,$$2}' $(MAKEFILE_LIST)
