# GPIO Blink — LicheeRV Nano

build:
	mkdir -p bin
	${COMPILER}/riscv64-unknown-linux-musl-gcc main.c -o ./bin/gpio_blink
	@echo "Build complete: bin/gpio_blink"

clean:
	rm -rf bin

.PHONY: build clean
