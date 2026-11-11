CXX ?= g++
CXXFLAGS ?= -std=c++17 -O3 -flto -Wall -Wextra -Wpedantic
LDFLAGS ?= -static-libstdc++ -static-libgcc
SRCS := $(wildcard src/*.cpp)
HEADERS := $(wildcard src/*.h)
BIN := bin/minivector

all: $(BIN)

$(BIN): $(SRCS) $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(SRCS)

asan: $(SRCS) $(HEADERS)
	@mkdir -p bin
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined \
		-fno-omit-frame-pointer -o bin/minivector-asan $(SRCS)

clean:
	rm -rf bin

.PHONY: all asan clean
