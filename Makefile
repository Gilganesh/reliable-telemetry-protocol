CC       ?= cc
SANITIZE ?= undefined
CFLAGS   ?= -Wall -Wextra -Werror -g -fsanitize=$(SANITIZE)
BUILD    := build

TESTS := test_protocol test_reliability test_packet_queue
CXX   ?= c++

all: gateway sim_node

gateway:
	$(MAKE) -C gateway

sim_node:
	$(MAKE) -C sim_node

$(BUILD)/test_%: tests/test_%.c protocol/protocol.c protocol/reliability.c protocol/*.h node_common/packet_queue.h | $(BUILD)
	$(CC) $(CFLAGS) -Iprotocol -Inode_common $< protocol/protocol.c protocol/reliability.c -o $@

$(BUILD)/test_lcd: tests/test_lcd.cpp node_common/node_lcd.h tests/stubs/*.h | $(BUILD)
	$(CXX) -std=gnu++17 -Wall -Wextra -Werror -g -fsanitize=$(SANITIZE) -Itests/stubs -Inode_common $< -o $@

$(BUILD):
	mkdir -p $@

test: $(addprefix $(BUILD)/,$(TESTS)) $(BUILD)/test_lcd
	@for t in $^; do ./$$t > $$t.log || { cat $$t.log; exit 1; }; echo "$$t: $$(tail -n 2 $$t.log | head -n 1)"; done
	@cd web && python3 protocol_codec.py
	@node_common/sync.sh --check

clean:
	rm -rf $(BUILD)
	$(MAKE) -C gateway clean
	$(MAKE) -C sim_node clean

.PHONY: all gateway sim_node test clean
