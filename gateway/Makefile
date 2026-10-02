CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2
BREW    := $(shell brew --prefix 2>/dev/null)
ifneq ($(BREW),)
  CFLAGS  += -I$(BREW)/include
  LDFLAGS += -L$(BREW)/lib
endif
CFLAGS += -I../protocol -pthread

SRC = gateway.c ../protocol/protocol.c

gateway: $(SRC) ../protocol/protocol.h
	$(CC) $(CFLAGS) $(SRC) $(LDFLAGS) -lmosquitto -lcjson -o gateway

clean:
	rm -f gateway

.PHONY: clean
