REBAR ?= ./rebar
REBAR_DEBUG=$(REBAR) -C rebar.debug.config
REBAR_COMPILE=$(REBAR) get-deps compile
REBAR_DEBUG_COMPILE=$(REBAR_DEBUG) get-deps compile
LAST_CONFIG:=$(shell cat config.tmp)
PLT=dialyzer/sqlite3.plt

all: config_normal compile

debug: config_debug
	$(REBAR_DEBUG_COMPILE) 

compile:
	$(REBAR) get-deps
	$(REBAR) compile
	
test:
	$(REBAR) get-deps
	$(REBAR) compile
	$(REBAR) eunit

clean:
	-rm -rf deps ebin priv/*.so doc/* .eunit/* c_src/*.o config.tmp

docs:
	$(REBAR) get-deps
	$(REBAR) compile
	$(REBAR) doc

static: config_debug
	$(REBAR_DEBUG_COMPILE)
ifeq ($(wildcard $(PLT)),)
	dialyzer --build_plt --apps kernel stdlib erts --output_plt $(PLT) 
else
	dialyzer --plt $(PLT) -r ebin
endif

cross_compile: config_cross
	$(REBAR_COMPILE) -C rebar.cross_compile.config

valgrind: config_debug
	$(REBAR_DEBUG_COMPILE)
	valgrind --tool=memcheck --leak-check=yes --num-callers=20 ./test.sh

ifeq ($(LAST_CONFIG),normal)
config_normal: ;
else
config_normal: clean
	rm -f config.tmp
	echo "normal" > config.tmp
endif

ifeq ($(LAST_CONFIG),debug)
config_debug: ;
else
config_debug: clean
	rm -f config.tmp
	echo "debug" > config.tmp
endif

ifeq ($(LAST_CONFIG),cross)
config_cross: ;
else
config_cross: clean
	rm -f config.tmp
	echo "cross" > config.tmp
endif

.PHONY: all compile test clean docs static valgrind config_normal config_debug config_cross
