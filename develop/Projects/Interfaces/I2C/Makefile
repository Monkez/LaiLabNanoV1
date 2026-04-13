# I2C AHT20 — LicheeRV Nano

build:
	mkdir -p bin
	${COMPILER}/riscv64-unknown-linux-musl-gcc main.c -o ./bin/i2c_aht20
	@echo "Build complete: bin/i2c_aht20"

clean:
	rm -rf bin

.PHONY: build clean
