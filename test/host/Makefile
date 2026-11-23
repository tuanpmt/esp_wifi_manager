# Host-side unit tests for pure (ESP-IDF independent) logic.
# Usage: make -C test/host

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined
SRC_DIR := ../../src

test: test_sm
	./test_sm

test_sm: test_sm.c $(SRC_DIR)/esp_wifi_manager_sm.c $(SRC_DIR)/esp_wifi_manager_sm.h
	$(CC) $(CFLAGS) -I$(SRC_DIR) -o $@ test_sm.c $(SRC_DIR)/esp_wifi_manager_sm.c

clean:
	rm -f test_sm

.PHONY: test clean
