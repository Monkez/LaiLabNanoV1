# HelloWorld - LicheeRV Nano
# Cross-compile with RISC-V musl GCC

build:
	mkdir -p bin
	@echo "Using compiler: ${COMPILER}"
	${COMPILER}/riscv64-unknown-linux-musl-gcc main.c -o ./bin/hello
	@echo "Build complete: bin/hello"

clean:
	rm -rf bin

.PHONY: build clean
