BUILD_DIR ?= build
PREFIX ?= /usr/local

.PHONY: all build configure clean install

all: build

configure:
	cmake -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$(PREFIX)

build: $(BUILD_DIR)/Makefile
	cmake --build $(BUILD_DIR) --parallel

$(BUILD_DIR)/Makefile:
	cmake -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$(PREFIX)

clean:
	rm -rf $(BUILD_DIR)

install: build
	cmake --install $(BUILD_DIR) --prefix $(PREFIX)
