CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2
BREW    := $(shell brew --prefix 2>/dev/null)
ifneq ($(BREW),)
  CFLAGS  += -I$(BREW)/include
  LDFLAGS += -L$(BREW)/lib
endif
CFLAGS += -I../protocol

SRC = sim_node.c ../protocol/protocol.c ../protocol/reliability.c

sim_node: $(SRC) ../protocol/protocol.h ../protocol/reliability.h
	$(CC) $(CFLAGS) $(SRC) $(LDFLAGS) -lmosquitto -o sim_node

clean:
	rm -f sim_node

.PHONY: clean
