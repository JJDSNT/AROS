ROOT = ../../../..
RIGEL_BUILD = /tmp/aros-m68kemu-rigel-host-rigel-release
RIGEL_LIB = $(RIGEL_BUILD)/librigel.a
M68K = $(ROOT)/rom/m68kemu/Rigel/scripts/m68k

CXXFLAGS = -std=c++20 -Wall -Wextra -Wno-unused-parameter -g \
           -Istubs -I$(ROOT)/rom/m68kemu -I$(ROOT)/rom/m68kemu/Moira
RIGEL_CPPFLAGS = -DM68KEMU_WITH_RIGEL=1 -Istubs \
                 -I$(ROOT)/rom/m68kemu \
                 -I$(ROOT)/rom/m68kemu/Rigel/include

TESTS = test_m68kemu test_moira_exec test_hello_thunks test_rigel_integration \
        test_rigel_chip_irq test_rigel_exec_irq test_rigel_display \
        test_rigel_input

all: $(TESTS)

# Tests #include the library sources directly
M68KEMU_SRCS = $(wildcard $(ROOT)/rom/m68kemu/*.c $(ROOT)/rom/m68kemu/*.cpp \
                          $(ROOT)/rom/m68kemu/*.h $(wildcard stubs/*/*.h stubs/*.h))
$(TESTS) m68kemu_rigel_host.o: $(M68KEMU_SRCS)

test_m68kemu: test_m68kemu.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

test_moira_exec: test_moira_exec.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

test_hello_thunks: test_hello_thunks.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

$(RIGEL_LIB):
	cmake -S $(ROOT)/rom/m68kemu/Rigel -B $(RIGEL_BUILD) \
		-DCMAKE_BUILD_TYPE=Release \
		-DRIGEL_BUILD_TESTS=OFF -DRIGEL_BUILD_HARNESS=OFF \
		-DRIGEL_ENABLE_SIMD=OFF
	cmake --build $(RIGEL_BUILD)

m68kemu_rigel_host.o: $(ROOT)/rom/m68kemu/m68kemu_rigel.c $(RIGEL_LIB)
	$(CC) -std=c11 -Wall -Wextra $(RIGEL_CPPFLAGS) -c -o $@ $<

test_rigel_integration: test_rigel_integration.cpp m68kemu_rigel_host.o $(RIGEL_LIB)
	$(CXX) $(CXXFLAGS) $(RIGEL_CPPFLAGS) -o $@ $< m68kemu_rigel_host.o $(RIGEL_LIB)

test_rigel_chip_irq: test_rigel_chip_irq.cpp m68k/rigel_chip_irq_hunk.h m68k/rigel_chip_irq_short_hunk.h m68kemu_rigel_host.o $(RIGEL_LIB)
	$(CXX) $(CXXFLAGS) $(RIGEL_CPPFLAGS) -o $@ $< m68kemu_rigel_host.o $(RIGEL_LIB)

test_rigel_exec_irq: test_rigel_exec_irq.cpp m68k/rigel_exec_irq_hunk.h m68kemu_rigel_host.o $(RIGEL_LIB)
	$(CXX) $(CXXFLAGS) $(RIGEL_CPPFLAGS) -o $@ $< m68kemu_rigel_host.o $(RIGEL_LIB)

test_rigel_display: test_rigel_display.cpp m68k/rigel_display_hunk.h m68kemu_rigel_host.o $(RIGEL_LIB)
	$(CXX) $(CXXFLAGS) $(RIGEL_CPPFLAGS) -o $@ $< m68kemu_rigel_host.o $(RIGEL_LIB)

test_rigel_input: test_rigel_input.cpp m68k/rigel_input_hunk.h m68kemu_rigel_host.o $(RIGEL_LIB)
	$(CXX) $(CXXFLAGS) $(RIGEL_CPPFLAGS) -o $@ $< m68kemu_rigel_host.o $(RIGEL_LIB)

# The 68k fixtures are committed so "make test" needs no m68k toolchain.
# Regenerating them needs vasm (docker pull amigadev/crosstools:m68k-amigaos).
fixtures:
	$(M68K) vasmm68k_mot -Fhunkexe -kick1hunks -nosym -quiet -o m68k/rigel_chip_irq.hunk m68k/rigel_chip_irq.s
	python3 m68k/bin_to_header.py m68k/rigel_chip_irq.hunk m68k/rigel_chip_irq_hunk.h rigel_chip_irq_hunk
	$(M68K) vasmm68k_mot -Fhunkexe -quiet -o m68k/rigel_chip_irq_short.hunk m68k/rigel_chip_irq.s
	python3 m68k/bin_to_header.py m68k/rigel_chip_irq_short.hunk m68k/rigel_chip_irq_short_hunk.h rigel_chip_irq_short_hunk
	$(M68K) vasmm68k_mot -Fhunkexe -quiet -o m68k/rigel_exec_irq.hunk m68k/rigel_exec_irq.s
	python3 m68k/bin_to_header.py m68k/rigel_exec_irq.hunk m68k/rigel_exec_irq_hunk.h rigel_exec_irq_hunk
	$(M68K) vasmm68k_mot -Fhunkexe -quiet -o m68k/rigel_display.hunk m68k/rigel_display.s
	python3 m68k/bin_to_header.py m68k/rigel_display.hunk m68k/rigel_display_hunk.h rigel_display_hunk
	$(M68K) vasmm68k_mot -Fhunkexe -quiet -o m68k/rigel_input.hunk m68k/rigel_input.s
	python3 m68k/bin_to_header.py m68k/rigel_input.hunk m68k/rigel_input_hunk.h rigel_input_hunk
	rm -f m68k/rigel_chip_irq.hunk m68k/rigel_chip_irq_short.hunk m68k/rigel_exec_irq.hunk m68k/rigel_display.hunk m68k/rigel_input.hunk

test: all
	./test_m68kemu
	./test_moira_exec
	./test_hello_thunks
	./test_rigel_integration
	./test_rigel_chip_irq
	./test_rigel_exec_irq
	./test_rigel_display
	./test_rigel_input

clean:
	rm -f $(TESTS) m68kemu_rigel_host.o

.PHONY: all test clean fixtures
