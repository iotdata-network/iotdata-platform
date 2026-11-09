##
## Host tests for the shared headers in iotdata-platform/include.
##
## They live here because the headers belong to no single project: their consumers are ESP-IDF
## applications a host toolchain cannot build, so this is the only place they get compiled with
## warnings on and actually run.
##

IOTDATA_SRC          = ../../..
IOTDATA_SRC_LIBRARY  = $(IOTDATA_SRC)/iotdata-library
IOTDATA_SRC_PLATFORM = $(IOTDATA_SRC)/iotdata-platform
IOTDATA_SRC_EXAMPLE  = $(IOTDATA_SRC)/iotdata-example
IOTDATA_SRC_BLACKBOX = $(IOTDATA_SRC)/iotdata-depend/blackbox/include
IOTDATA_SRC_HPATCH   = $(IOTDATA_SRC)/iotdata-depend/hpatchlite

## a variant has to be named: the library includes no default by design
IOTDATA_VARIANT ?= $(abspath $(IOTDATA_SRC_PLATFORM)/include/iotdata_variant_weather_station.h)

TEST_CC     ?= gcc
## _DEFAULT_SOURCE because -std=c11 is strict ISO, which hides POSIX from the system headers --
## iotdata_node_utils.h calls gethostname() to derive a node identity on a host, and anything that
## includes it needs the declaration. Set once here rather than per test, since the reach spreads.
TEST_CFLAGS ?= -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Wconversion -Wshadow \
               -I$(IOTDATA_SRC_LIBRARY) -I$(IOTDATA_SRC_PLATFORM)/include -I$(IOTDATA_SRC_EXAMPLE)/include \
               -DIOTDATA_VARIANT='"$(IOTDATA_VARIANT)"' -DIOTDATA_NO_JSON
TEST_DIR    ?= build_test
TEST_VERSION_DEFS ?= -DIOTDATA_NODE_VERSION_APP='"testapp"' -DIOTDATA_NODE_VERSION_SEMVER='"1.2.3"' -DIOTDATA_NODE_VERSION_STAMP='"$(shell date -u +%Y%m%d%H%M)"'

.PHONY: test test-clean

test:
	@mkdir -p $(TEST_DIR)
	@$(TEST_CC) $(TEST_CFLAGS) test_node_runtime.c $(IOTDATA_SRC_LIBRARY)/iotdata.c -o $(TEST_DIR)/test_node_runtime -lm
	@$(TEST_DIR)/test_node_runtime
# JSON ON for this one: -UIOTDATA_NO_JSON and cJSON, because the version header's encoders are
# only compiled when a project has JSON, and a test that skips them tests nothing.
	@$(TEST_CC) $(TEST_CFLAGS) -UIOTDATA_NO_JSON -DPLATFORM_LINUX $(TEST_VERSION_DEFS) test_version.c -o $(TEST_DIR)/test_version -lm -lcjson
	@$(TEST_DIR)/test_version
# JSON on: the point of this suite is that the two media agree, so it needs both of them.
# built both ways: the disabled build is the one that used to be a per-app deletion
	@$(TEST_CC) $(TEST_CFLAGS) -DIOTDATA_DIAGNOSTICS=1 -I$(IOTDATA_SRC_BLACKBOX) test_node_diagnostics.c -o $(TEST_DIR)/test_node_diagnostics_on -lm
	@$(TEST_DIR)/test_node_diagnostics_on
	@$(TEST_CC) $(TEST_CFLAGS) -DIOTDATA_DIAGNOSTICS=0 -I$(IOTDATA_SRC_BLACKBOX) test_node_diagnostics.c -o $(TEST_DIR)/test_node_diagnostics_off -lm
	@$(TEST_DIR)/test_node_diagnostics_off
	@$(TEST_CC) $(TEST_CFLAGS) -UIOTDATA_NO_JSON test_node_status.c -o $(TEST_DIR)/test_node_status -lm -lcjson
	@$(TEST_DIR)/test_node_status
# the filestore SEAM lives in include/device now, and content.h is written against it
	@$(TEST_CC) $(TEST_CFLAGS) -I$(IOTDATA_SRC_PLATFORM)/include/device test_node_content_ota.c -o $(TEST_DIR)/test_node_content_ota -lm
	@$(TEST_DIR)/test_node_content_ota
# both ways, because the table is a size/speed knob and an untested alternative implementation is
# worse than no alternative. -ldl: the suite dlopens liblzma to check itself against the reference.
	@$(TEST_CC) $(TEST_CFLAGS) -I$(IOTDATA_SRC_PLATFORM)/include/device test_node_crc64.c -o $(TEST_DIR)/test_node_crc64 -ldl
	@$(TEST_DIR)/test_node_crc64
	@$(TEST_CC) $(TEST_CFLAGS) -I$(IOTDATA_SRC_PLATFORM)/include/device -D_IOTDATA_CRC64_TABLE=0 test_node_crc64.c -o $(TEST_DIR)/test_node_crc64_bitwise -ldl
	@$(TEST_DIR)/test_node_crc64_bitwise
# the RAM backend lives in include/device, so this needs that on the path as the cart test does
	@$(TEST_CC) $(TEST_CFLAGS) -I$(IOTDATA_SRC_PLATFORM)/include/device test_node_content.c -o $(TEST_DIR)/test_node_content -lm
	@$(TEST_DIR)/test_node_content
# again with a table smaller than the store, which is the only way the ownership overflow -- a
# misdimensioning, counted rather than hidden -- is reachable at all
	@$(TEST_CC) $(TEST_CFLAGS) -I$(IOTDATA_SRC_PLATFORM)/include/device -DIOTDATA_NODE_CONTENT_OWNED_MAX=4 test_node_content.c -o $(TEST_DIR)/test_node_content_tight -lm
	@$(TEST_DIR)/test_node_content_tight
# the DIRECTORY backend, against a real filesystem -- and driven side by side with the exact RAM
# model, which is the comparison that backend was written for and had no host counterpart until now
	@$(TEST_CC) $(TEST_CFLAGS) -I$(IOTDATA_SRC_PLATFORM)/include/device test_node_filestore_linux.c -o $(TEST_DIR)/test_node_filestore_linux -lm
	@$(TEST_DIR)/test_node_filestore_linux
# the OTA CONSUMER, with a scripted slot and a scripted digest -- which is why claim decisions, the
# write failures and the attempt counter across a reboot are all reachable without a cable
# -lz: the suite inflates a REAL raw-deflate stream through d_module_inflate_linux.h, rather than
# standing in for one -- see the note in that header
	@$(TEST_CC) $(TEST_CFLAGS) -I$(IOTDATA_SRC_PLATFORM)/include/device -I$(IOTDATA_SRC_HPATCH) test_node_content_ota_apply.c -o $(TEST_DIR)/test_node_content_ota_apply -lm -lz
	@$(TEST_DIR)/test_node_content_ota_apply
# the OFFERS table: the manifest half, which is the store's protocol-aware counterpart. Needs the
# library on the path as well as the seam, because a manifest IS protocol -- its codec, its expiry
# sentinels and its revocation rule all live in iotdata_node.h.
	@$(TEST_CC) $(TEST_CFLAGS) -I$(IOTDATA_SRC_PLATFORM)/include/device test_node_content_manifest.c $(IOTDATA_SRC_LIBRARY)/iotdata.c -o $(TEST_DIR)/test_node_content_manifest -lm
	@$(TEST_DIR)/test_node_content_manifest
# again with a table of two rows, which is the only way the full-table refusal is reachable
	@$(TEST_CC) $(TEST_CFLAGS) -I$(IOTDATA_SRC_PLATFORM)/include/device -DIOTDATA_NODE_CONTENT_MANIFESTS_MAX=2 test_node_content_manifest.c $(IOTDATA_SRC_LIBRARY)/iotdata.c -o $(TEST_DIR)/test_node_content_manifest_tight -lm
	@$(TEST_DIR)/test_node_content_manifest_tight
# A MANIFEST'S WHOLE LIFE, across four nodes on a fake air, with real frames: authored in two
# places, advertised, heard or ignored for each of the four reasons, pulled, checked against the
# offer that described it, superseded, revoked. The receive branch in that suite is the one the
# applications will call, written where it can be driven.
# READVERTISE_MIN_S=0: this suite is about REACH -- who hears an offer and who does not -- and it
# re-originates several times within a simulated second to get there. The floor under
# re-origination has its own tests in test_node_content_manifest.c, where the clock is explicit.
	@$(TEST_CC) $(TEST_CFLAGS) -I$(IOTDATA_SRC_PLATFORM)/include/device -DIOTDATA_NODE_CONTENT_READVERTISE_MIN_S=0 test_node_content_lifecycle.c $(IOTDATA_SRC_LIBRARY)/iotdata.c -o $(TEST_DIR)/test_node_content_lifecycle -lm
	@$(TEST_DIR)/test_node_content_lifecycle
# the console and CONTROL words over that same backend. The one suite here that catches stdout,
# because for a download the reply IS the protocol that esp32-tool parses.
	@$(TEST_CC) $(TEST_CFLAGS) -I$(IOTDATA_SRC_PLATFORM)/include/device test_node_console_content.c -o $(TEST_DIR)/test_node_console_content -lm
	@$(TEST_DIR)/test_node_console_content
	@$(TEST_CC) $(TEST_CFLAGS) test_node_config_blocks.c -o $(TEST_DIR)/test_node_config_blocks -lm
	@$(TEST_DIR)/test_node_config_blocks
	@$(TEST_CC) $(TEST_CFLAGS) test_node_utils.c -o $(TEST_DIR)/test_node_utils -lm
	@$(TEST_DIR)/test_node_utils
	@$(TEST_CC) $(TEST_CFLAGS) test_node_stations.c -o $(TEST_DIR)/test_node_stations -lm
	@$(TEST_DIR)/test_node_stations
	@$(TEST_CC) $(TEST_CFLAGS) test_node_console.c -o $(TEST_DIR)/test_node_console -lm
	@$(TEST_DIR)/test_node_console
	@$(TEST_CC) $(TEST_CFLAGS) -UIOTDATA_NO_JSON test_node_control.c -o $(TEST_DIR)/test_node_control -lm -lcjson
	@$(TEST_DIR)/test_node_control
	@$(TEST_CC) $(TEST_CFLAGS) test_format.c -o $(TEST_DIR)/test_format
	@$(TEST_DIR)/test_format
	@$(TEST_CC) $(TEST_CFLAGS) test_module_buffers.c -o $(TEST_DIR)/test_module_buffers
	@$(TEST_DIR)/test_module_buffers
	@$(TEST_CC) $(TEST_CFLAGS) -D_GNU_SOURCE -pthread test_module_buffers_locked.c -o $(TEST_DIR)/test_module_buffers_locked
	@$(TEST_DIR)/test_module_buffers_locked
	@$(TEST_CC) $(TEST_CFLAGS) -D_GNU_SOURCE test_node_state.c -o $(TEST_DIR)/test_node_state -lm
	@$(TEST_DIR)/test_node_state
	@$(TEST_CC) $(TEST_CFLAGS) -D_GNU_SOURCE test_node_config.c -o $(TEST_DIR)/test_node_config -lm
	@$(TEST_DIR)/test_node_config
	@$(TEST_CC) $(TEST_CFLAGS) -D_GNU_SOURCE test_node_config_mesh.c -o $(TEST_DIR)/test_node_config_mesh -lm
	@$(TEST_DIR)/test_node_config_mesh
	@$(TEST_CC) $(TEST_CFLAGS) -D_GNU_SOURCE test_node_config_console.c -o $(TEST_DIR)/test_node_config_console -lm
	@$(TEST_DIR)/test_node_config_console
	@$(TEST_CC) $(TEST_CFLAGS) -D_GNU_SOURCE test_node_config_string.c -o $(TEST_DIR)/test_node_config_string -lm
	@$(TEST_DIR)/test_node_config_string
	@$(TEST_CC) $(TEST_CFLAGS) -D_GNU_SOURCE test_node_settings_console.c -o $(TEST_DIR)/test_node_settings_console -lm
	@$(TEST_DIR)/test_node_settings_console
# a device header, which is unusual here: it is testable on the host because its whole seam is one
# gpio pair and a clock, both of which a test can own outright.
	@$(TEST_CC) $(TEST_CFLAGS) -I$(IOTDATA_SRC_PLATFORM)/include/device test_interface_cart.c -o $(TEST_DIR)/test_interface_cart -lm
	@$(TEST_DIR)/test_interface_cart
# another device header, and the only one here that links a third-party library: the subscriber
# table routes with mosquitto's own topic matcher, which is pure string work -- no broker, no
# connection, nothing started. -lmosquitto is for that one function.
	@$(TEST_CC) $(TEST_CFLAGS) -I$(IOTDATA_SRC_PLATFORM)/include/device test_module_mqtt_linux.c -o $(TEST_DIR)/test_module_mqtt_linux -lm -lmosquitto
	@$(TEST_DIR)/test_module_mqtt_linux

test-clean:
	@rm -rf $(TEST_DIR)
