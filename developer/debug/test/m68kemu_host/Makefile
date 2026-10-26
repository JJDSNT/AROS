ROOT = ../../../..
RIGEL_BUILD = /tmp/aros-m68kemu-rigel-host-rigel
RIGEL_LIB = $(RIGEL_BUILD)/librigel.a

CXXFLAGS = -std=c++20 -Wall -Wextra -Wno-unused-parameter -g \
           -Istubs -I$(ROOT)/rom/m68kemu -I$(ROOT)/rom/m68kemu/Moira
RIGEL_CPPFLAGS = -DM68KEMU_WITH_RIGEL=1 -Istubs \
                 -I$(ROOT)/rom/m68kemu \
                 -I$(ROOT)/rom/m68kemu/Rigel/include

TESTS = test_m68kemu test_moira_exec test_hello_thunks test_rigel_integration

all: $(TESTS)

test_m68kemu: test_m68kemu.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

test_moira_exec: test_moira_exec.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

test_hello_thunks: test_hello_thunks.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

$(RIGEL_LIB):
	cmake -S $(ROOT)/rom/m68kemu/Rigel -B $(RIGEL_BUILD) \
		-DRIGEL_BUILD_TESTS=OFF -DRIGEL_BUILD_HARNESS=OFF \
		-DRIGEL_ENABLE_SIMD=OFF
	cmake --build $(RIGEL_BUILD)

m68kemu_rigel_host.o: $(ROOT)/rom/m68kemu/m68kemu_rigel.c $(RIGEL_LIB)
	$(CC) -std=c11 -Wall -Wextra $(RIGEL_CPPFLAGS) -c -o $@ $<

test_rigel_integration: test_rigel_integration.cpp m68kemu_rigel_host.o $(RIGEL_LIB)
	$(CXX) $(CXXFLAGS) $(RIGEL_CPPFLAGS) -o $@ $< m68kemu_rigel_host.o $(RIGEL_LIB)

test: all
	./test_m68kemu
	./test_moira_exec
	./test_hello_thunks
	./test_rigel_integration

clean:
	rm -f $(TESTS) m68kemu_rigel_host.o

.PHONY: all test clean
