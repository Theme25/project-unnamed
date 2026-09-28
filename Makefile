# Strict IEEE-754 double semantics are mandatory for bit-exact Flash parity:
#  -ffp-contract=off   never fuse a*b+c into FMA
#  -fno-fast-math      (default, stated for clarity)
#  x86-64 uses SSE2 scalar doubles (no x87 extended precision)
CXX      ?= g++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -ffp-contract=off -fno-fast-math -fexcess-precision=standard
SRC = src/b2collision.cpp src/b2world.cpp src/redball.cpp src/rbsim.cpp
HDR = src/b2math.h src/b2world.h src/redball.h

rbsim: $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

test: rbsim
	./rbsim test

clean:
	rm -f rbsim
.PHONY: test clean
