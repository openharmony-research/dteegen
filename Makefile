.PHONY: all debug clean perf deploy generate test_project generate_cpp build_in_docker docker build_compile_deps run_docker build_target build_target_raw

all:
	./scripts/build_dteegen.sh release

debug:
	./scripts/build_dteegen.sh debug

clean:
	rm -rf build

perf:
	sudo perf record --call-graph dwarf ./build/dteegen ./test/test_seal

generate: all
	./build/dteegen test_project

generate_cpp: all
	./build/dteegen test_project_cpp

deploy: 
	tar -zcvf generated.tar.gz generated
	scp -P 12055 generated.tar.gz root@localhost:~/

test_project: 
	bash ./scripts/build_test_project.sh

build_in_docker:
	bash ./scripts/build_in_docker.sh

build_compile_deps:
	bash ./scripts/build_compile_deps.sh
docker:
	bash ./scripts/update_docker_deps.sh
	./scripts/build_dteegen.sh debug riscv64
	docker build --network=host -t rv-secgear .
run_docker:
	docker run -v $(shell pwd):/test -v /usr/bin/qemu-riscv64-static:/usr/bin/qemu-riscv64-static --network=host -w /test -it rv-secgear


build_target: 
	@echo "Building $(TARGET)"
	make build_compile_deps
	sudo make docker
	sudo ./scripts/gen_target.sh $(TARGET)
	sudo ./scripts/build_in_docker.sh $(TARGET).generated

build_target_raw: 
	@echo "Building $(TARGET)"
	sudo ./scripts/gen_target.sh $(TARGET)
	sudo ./scripts/build_in_docker.sh $(TARGET).generated

