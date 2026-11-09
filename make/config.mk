# config.mk — shared build configuration for all iotdata repos.
#
# Single source of truth for where the iotdata sources live. Included by each
# repo's Makefiles (and, for esp32 builds, pushed through to CMake) via a sibling-
# relative path, e.g.:
#   iotdata-example/Makefile                 -> include ../iotdata-platform/make/config.mk
#   iotdata-example/simulator/Makefile       -> include ../../iotdata-platform/make/config.mk
#   iotdata-device/tsa/test_esp32c3/Makefile -> include ../../../iotdata-platform/make/config.mk
#
# Resolution order for the source root:
#   1. $(IOTDATA_SRC)        — explicit override (environment or `make IOTDATA_SRC=...`)
#   2. $(IOTDATA_APEX)/src
# IOTDATA_APEX defaults to this tree's apex, derived from THIS file's own location
# (it lives at $(IOTDATA_APEX)/src/iotdata-platform/make), so everything self-locates
# with no hardcoded paths. Set IOTDATA_APEX or IOTDATA_SRC (env or command line) to
# relocate.
#
# NB: keep comments on their own lines — a trailing "# ..." after a value would
# leave the spaces before the # in the variable, corrupting the path.

# Directory of this file (.../iotdata-platform/make), captured at include time.
_IOTDATA_MK_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

# Apex of the iotdata tree (three levels up: make -> iotdata-platform -> src -> apex).
IOTDATA_APEX ?= $(abspath $(_IOTDATA_MK_DIR)/../../..)

# Hostname we're building on
IOTDATA_HOST ?= $(shell hostname -s)

# Source root holding all the iotdata-* repos: explicit override, else <apex>/src.
IOTDATA_SRC ?= $(IOTDATA_APEX)/src
# Force absolute. A project may set IOTDATA_SRC=../.. (fine for native gcc -I from the project dir),
# but the esp32 CMake component build resolves relative include dirs from main/ (one level deeper)
# and would double them (…/iotdata-example/iotdata-example/include). Absolute paths avoid that and
# work for both. $(abspath) resolves against the invoking Makefile's directory.
IOTDATA_SRC := $(abspath $(IOTDATA_SRC))

# Repo roots — every iotdata repo is a sibling under $(IOTDATA_SRC).
IOTDATA_SRC_LIBRARY  ?= $(IOTDATA_SRC)/iotdata-library
IOTDATA_SRC_DEPEND   ?= $(IOTDATA_SRC)/iotdata-depend
IOTDATA_SRC_PLATFORM ?= $(IOTDATA_SRC)/iotdata-platform
IOTDATA_SRC_DEVICE   ?= $(IOTDATA_SRC)/iotdata-device
IOTDATA_SRC_EXAMPLE  ?= $(IOTDATA_SRC)/iotdata-example
IOTDATA_SRC_DEPLOY   ?= $(IOTDATA_SRC)/iotdata-deploy

# ------------------------------------------------------------------------------------------------
# HOST-LOCAL MAKE VARIABLES -- the same convention as iotdata_config.<host>.h,
# sdkconfig.<host>.defaults, <target>.<host>.cfg and lab.<host>.conf: one gitignored file per box,
# found by hostname, never committed. This is the make-variable member of that set, which was the
# one missing.
#
# WHY IT EXISTS. A knob like NO_SLEEP_DEEP=1 or AE_SDC_CARRIER=0 is true of a DESK, not of the
# project. Setting one by editing the committed Makefile means it sits in every diff from then on,
# gets committed by accident eventually, and travels to a box where it is wrong -- and NO_SLEEP_DEEP
# is exactly the one you least want shipped, which is why the firmware says DO NOT SHIP at boot.
#
# THE ORDER IS THE PRECEDENCE, and that is the whole trick. Project knobs are declared `?=`, which
# assigns only if the name is still unset -- so the FIRST assignment wins, not the last. These are
# read here, before any project Makefile reaches its own `?=` lines, which turns those lines into
# the fallbacks they were always written as. Project-local is read before the tree-wide one for the
# same reason: more specific, earlier, wins. A line that must beat even a command-line assignment
# says `override`.
#
#   iotdata-example/sensor_lora_esp32/Makefile.iotdata-master.defs   this project, this box
#   Makefile.iotdata-master.defs (beside src/Makefile)               every project, this box
IOTDATA_DEFS := $(wildcard Makefile.$(IOTDATA_HOST).defs $(IOTDATA_SRC)/Makefile.$(IOTDATA_HOST).defs)
-include $(IOTDATA_DEFS)

# ------------------------------------------------------------------------------------------------
# THE DEPLOYMENT -- which site's release tree a build stages into.
#
# A release is staged FOR A SITE, not for the world. Each deployment keeps its own tree at
# iotdata-deploy/<deployment>/release, in the same one-subtree-per-platform shape the packaging
# rules expect, so staging, packaging and OTA bundling all work there exactly as they did in the
# single shared tree. $(IOTDATA_DEPLOYMENT) names which one: normally exported from .bashrc, or
# per-invocation with `make stage IOTDATA_DEPLOYMENT=se-67194`.
#
# THERE IS NO DEFAULT, DELIBERATELY. A release staged into the wrong site's tree is
# indistinguishable from a correct one afterwards -- same name, same manifest, same bytes -- and the
# first evidence would be which fleet got the update. A default would make that the quiet case, so
# the staging rules refuse instead.
#
# They refuse IN THE RECIPE, not here: an unset deployment must not break a build that is only
# compiling, and `make all` has nothing to do with where a release would go.
IOTDATA_DEPLOYMENT ?=
IOTDATA_DEPLOYMENT_DIR     = $(IOTDATA_SRC_DEPLOY)/$(IOTDATA_DEPLOYMENT)
IOTDATA_DEPLOYMENT_RELEASE = $(IOTDATA_DEPLOYMENT_DIR)/release
# One line, expanded into a recipe by whoever needs the deployment to be named. It lists what is
# actually there, because the answer to "it is not set" is nearly always one of two or three names
# and nobody remembers which spelling.
IOTDATA_DEPLOYMENT_ASSERT = \
    test -n "$(IOTDATA_DEPLOYMENT)" || { \
      echo "IOTDATA_DEPLOYMENT is not set -- say which deployment to stage into:" >&2; \
      echo "    export IOTDATA_DEPLOYMENT=<name>      (normally in .bashrc)" >&2; \
      echo "    make stage IOTDATA_DEPLOYMENT=<name>  (one build only)" >&2; \
      echo "  deployments in $(IOTDATA_SRC_DEPLOY): $$(ls -1 $(IOTDATA_SRC_DEPLOY) 2>/dev/null | tr '\n' ' ')" >&2; \
      exit 1; }

# iotdata-example internal sub-paths.
#   IOTDATA_SRC_EXAMPLE_SIMULATOR -> simulator sources
# (the variant-suite header is now canonical at IOTDATA_VARIANT_HEADER, below.)
IOTDATA_SRC_EXAMPLE_SIMULATOR ?= $(IOTDATA_SRC_EXAMPLE)/simulator

# The iotdata library's own sources — list in a project's SOURCES for rebuild-on-change + format.
# (iotdata.c is #included by the unity-build apps, so this is a prerequisite list, not a link list.)
IOTDATA_SRC_LIBRARY_SOURCES ?= \
    $(IOTDATA_SRC_LIBRARY)/iotdata_mesh.h \
    $(IOTDATA_SRC_LIBRARY)/iotdata.h \
    $(IOTDATA_SRC_LIBRARY)/iotdata.c

# The variant-suite the apps compile against, as a FILE path. Injected as -DIOTDATA_VARIANT so the
# committed wrapper iotdata-platform/include/iotdata_variant.h #includes it; override per project or
# per host to swap the variant set. No default is pulled in if unset -> the build breaks explicitly.
IOTDATA_VARIANT ?= $(IOTDATA_SRC_PLATFORM)/include/iotdata_variant_weather_station.h

# Shared native-build compiler flags. A Makefile uses these and may extend them, either
# inline (CFLAGS_COMMON = $(IOTDATA_CFLAGS_COMMON) -Wextra-local) or by appending after the
# include (IOTDATA_CFLAGS_COMMON += -Wextra-local). ?= so a project may pre-set to replace.
IOTDATA_CFLAGS_COMMON ?= \
    -Wfloat-conversion -Werror=float-conversion \
    -Wall -Wextra -Werror -Wpedantic \
    -Wstrict-prototypes -Wold-style-definition \
    -Wcast-align -Wcast-qual -Wconversion \
    -Wfloat-equal -Wformat=2 -Wformat-security \
    -Winit-self -Wjump-misses-init \
    -Wlogical-op -Wmissing-include-dirs \
    -Wnested-externs -Wpointer-arith \
    -Wredundant-decls -Wshadow \
    -Wstrict-overflow=2 -Wswitch-default \
    -Wunreachable-code -Wunused \
    -Wwrite-strings \
    -Wdouble-promotion \
    -Wnull-dereference \
    -Wduplicated-cond \
    -Wduplicated-branches \
    -Wrestrict \
    -Wstringop-overflow \
    -Wundef \
    -Wvla \
    -Wno-duplicated-branches
IOTDATA_CFLAGS_OPT ?= -O3

# Link deps of the iotdata library itself: cjson (JSON encode/decode) + m (float math). A Makefile
# appends its own, e.g. LIBS = $(IOTDATA_LIBS_COMMON) -lmosquitto -lpthread. LDFLAGS is a slot for
# shared linker options (none by default); extend locally the same way.
IOTDATA_LIBS_COMMON    ?= -lcjson -lm
IOTDATA_LDFLAGS_COMMON ?=

# The clang-format binary every repo's `format` target runs.
FORMAT ?= clang-format-22
