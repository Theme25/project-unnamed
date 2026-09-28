# Strict IEEE-754 double semantics are mandatory for bit-exact Flash parity:
#  -ffp-contract=off   never fuse a*b+c into FMA
#  -fno-fast-math      (default, stated for clarity)
#  x86-64 uses SSE2 scalar doubles (no x87 extended precision)
CXX      ?= g++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -ffp-contract=off -fno-fast-math -fexcess-precision=standard
SRC = src/libm_intel.S src/b2collision.cpp src/b2world.cpp src/b2joints.cpp src/redball.cpp src/rbsim.cpp src/verify.cpp
HDR = src/b2math.h src/b2world.h src/redball.h src/levels_data.h

rbsim: $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

tools/trig_flip_search: tools/trig_flip_search.cpp src/libm_intel.S src/b2collision.cpp src/b2world.cpp src/b2joints.cpp src/redball.cpp $(HDR)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ tools/trig_flip_search.cpp src/libm_intel.S src/b2collision.cpp src/b2world.cpp src/b2joints.cpp src/redball.cpp

test: rbsim
	./rbsim test

clean:
	rm -f rbsim tools/trig_flip_search
# Regenerate src/libm_intel.S from OpenJDK sources (needs network):
#   make libm OPENJDK=/path/to/dir-with-stubGenerator_x86_64_{sin,cos,constants}.cpp
libm:
	python3 tools/hotspot2gas.py $(OPENJDK) > src/libm_intel.S

# Regenerate src/levels_data.h from the SWF (needs JPEXS FFDec: -swf2xml):
#   make levels SWFXML=practice_full.xml
levels:
	python3 tools/extract_levels.py $(SWFXML) > levels.json
	python3 tools/gen_levels_data.py levels.json > src/levels_data.h

.PHONY: test clean libm levels
