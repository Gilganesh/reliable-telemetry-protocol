CC       ?= cc
SANITIZE ?= undefined
CFLAGS   ?= -Wall -Wextra -Werror -g -fsanitize=$(SANITIZE)
BUILD    := build

TESTS := test_protocol test_reliability test_packet_queue test_frame_reader test_threshold
CXX   ?= c++

all: gateway sim_node

gateway:
	$(MAKE) -C gateway

sim_node:
	$(MAKE) -C node/simulated

$(BUILD)/test_%: tests/test_%.c protocol/protocol.c protocol/reliability.c protocol/*.h node/_shared/packet_queue.h node/_shared/threshold.h gateway/frame_reader.h | $(BUILD)
	$(CC) $(CFLAGS) -Iprotocol -Inode/_shared -Igateway $< protocol/protocol.c protocol/reliability.c -o $@

$(BUILD)/test_lcd: tests/test_lcd.cpp node/_shared/node_lcd.h tests/stubs/*.h | $(BUILD)
	$(CXX) -std=gnu++17 -Wall -Wextra -Werror -g -fsanitize=$(SANITIZE) -Itests/stubs -Inode/_shared $< -o $@

$(BUILD):
	mkdir -p $@

test: $(addprefix $(BUILD)/,$(TESTS)) $(BUILD)/test_lcd
	@for t in $^; do ./$$t > $$t.log || { cat $$t.log; exit 1; }; echo "$$t: $$(tail -n 2 $$t.log | head -n 1)"; done
	@cd web && python3 protocol_codec.py
	@node/_shared/sync.sh --check

clean:
	rm -rf $(BUILD)
	$(MAKE) -C gateway clean
	$(MAKE) -C node/simulated clean

.PHONY: all gateway sim_node test clean
