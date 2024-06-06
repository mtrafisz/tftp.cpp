libname := tftpc
cc := g++
cflags := -Wall -Wextra -pedantic -std=c++11
test_dir := test
src_dir := src
comp_dir := bin
libs := stdc++
output_ext_static := a

src_files := $(wildcard $(src_dir)/*.cpp)
obj_files := $(patsubst $(src_dir)/%.cpp,$(comp_dir)/%.o,$(src_files))
test_files := $(wildcard $(test_dir)/*.cpp)
test_bins := $(patsubst $(test_dir)/%.cpp,$(comp_dir)/%,$(test_files))

ifeq ($(OS),Windows_NT)
	clean_cmd := del /f /q
	libs += ws2_32
	output_ext_shared := dll
else
	clean_cmd := rm -f
	output_ext_shared := so
endif

output_static := $(comp_dir)/lib$(libname).$(output_ext_static)
output_shared := $(comp_dir)/lib$(libname).$(output_ext_shared)

all: $(comp_dir) $(output_static) $(output_shared) test

$(comp_dir):
	mkdir $(comp_dir)

debug: cflags += -g
debug: all

$(output_static): $(obj_files)
	ar rcs $@ $^
	ranlib $@

$(output_shared): $(obj_files)
	$(cc) $(cflags) -fPIC -shared -o $@ $^ $(addprefix -l,$(libs))

$(comp_dir)/%.o: $(src_dir)/%.cpp
	$(cc) $(cflags) -c -o $@ $<

$(comp_dir)/%: $(test_dir)/%.cpp $(output_static)
	$(cc) $(cflags) -o $@ $< $(output_static) $(addprefix -l,$(libs))
	
test: $(test_bins)

.PHONY: all debug test
