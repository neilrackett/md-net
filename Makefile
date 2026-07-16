# Version from file
VERSION := $(shell cat version.txt)
DEFAULT_APP_UUID_KEY := 44444444-4444-4444-8444-444444444444
UUID_FROM_FILE := $(strip $(shell [ -f uuid.txt ] && tr -d '\r\n' < uuid.txt))
APP_UUID_KEY_RESOLVED := $(if $(strip $(APP_UUID_KEY)),$(strip $(APP_UUID_KEY)),$(if $(UUID_FROM_FILE),$(UUID_FROM_FILE),$(DEFAULT_APP_UUID_KEY)))
UART_BAUD ?= 115200
UART_DEV ?=

export STCMD_NO_TTY = 1

## Build production firmware for pico_w. Does NOT bump the version — it releases
## version.txt as-is (set the version there / bump manually before releasing).
## Uses APP_UUID_KEY env var, then uuid.txt, then default.
.PHONY: build
build:
	@echo "Using APP_UUID_KEY: $(APP_UUID_KEY_RESOLVED)"
	SKIP_VERSION_BUMP=1 ./build.sh pico_w release "$(APP_UUID_KEY_RESOLVED)"

## Build debug firmware for pico_w using APP_UUID_KEY env var, then uuid.txt, then default
.PHONY: debug
debug:
	@echo "Using APP_UUID_KEY: $(APP_UUID_KEY_RESOLVED)"
	./build.sh pico_w debug "$(APP_UUID_KEY_RESOLVED)"

## Tag this version
.PHONY: tag
tag:
	git tag $(VERSION) && git push origin $(VERSION) && \
	echo "Tagged: $(VERSION)"

## List candidate UART devices (macOS + Linux)
.PHONY: uart-list
uart-list:
	@for p in /dev/cu.usbmodem* /dev/cu.usbserial* /dev/tty.usbmodem* /dev/tty.usbserial* /dev/ttyACM* /dev/ttyUSB* /dev/cu.J* /dev/tty.J*; do \
		[ -e "$$p" ] && echo "$$p"; \
	done

## Open UART console on the first matching serial device using screen.
## Override with: make uart UART_DEV=/dev/cu.usbmodemXXXX
.PHONY: uart
uart:
	@dev="$(UART_DEV)"; \
	if [ -z "$$dev" ]; then \
		dev="$$(for p in /dev/cu.usbmodem* /dev/cu.usbserial* /dev/tty.usbmodem* /dev/tty.usbserial* /dev/ttyACM* /dev/ttyUSB* /dev/cu.J* /dev/tty.J*; do [ -e "$$p" ] && echo "$$p"; done | head -n 1)"; \
	fi; \
	if [ -z "$$dev" ]; then \
		echo "No serial device found. Run: make uart-list"; \
		exit 1; \
	fi; \
	echo "Opening UART on $$dev at $(UART_BAUD) baud"; \
	echo "Detach with Ctrl-A then K (or Ctrl-A then \\)"; \
	screen "$$dev" $(UART_BAUD)
