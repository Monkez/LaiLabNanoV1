# PWM Sweep — LicheeRV Nano

build:
	mkdir -p bin
	${COMPILER}/riscv64-unknown-linux-musl-gcc main.c -o ./bin/pwm_sweep
	@echo "Build complete: bin/pwm_sweep"

clean:
	rm -rf bin

.PHONY: build clean
