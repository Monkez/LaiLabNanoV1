# Copy GPIO header from GPIO project (shared library)
CFLAGS=-I../GPIO

build:
	mkdir -p bin
	${COMPILER}/riscv64-unknown-linux-musl-gcc ${CFLAGS} main.c -o ./bin/gpio_interrupt
	@echo "Build complete: bin/gpio_interrupt"

clean:
	rm -rf bin

.PHONY: build clean
