# Збірка і запуск Gateway з цієї папки:  make && ./gateway
# Протокол лежить у ./protocol/. Потрібні libmosquitto і libcjson
# (macOS: brew install mosquitto cjson).
CC      ?= cc
CFLAGS  ?= -Wall -Wextra
BREW    := $(shell brew --prefix 2>/dev/null)
ifneq ($(BREW),)
  CFLAGS += -I$(BREW)/include
  LDFLAGS += -L$(BREW)/lib
endif
CFLAGS += -Iprotocol

SRC = gateway.c protocol/protocol.c protocol/reliability.c

gateway: $(SRC) protocol/protocol.h protocol/reliability.h
	$(CC) $(CFLAGS) $(SRC) $(LDFLAGS) -lmosquitto -lcjson -o gateway

clean:
	rm -f gateway

.PHONY: clean
