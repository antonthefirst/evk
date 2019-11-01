CC:=g++
CFLAGS:=-Wall -std=c++0x -Wno-unknown-pragmas -fpermissive
SRCS:=$(wildcard core/*.cpp)
SRCS+=$(wildcard src/*.cpp)
SRCS+=$(wildcard wrap/*.cpp)
SRCS+=$(wildcard libs/imgui/*.cpp)
INCLUDES:=-I./libs -I. -ldl -lglfw
PROGRAM:=evk

$(PROGRAM):	$(SRCS) Makefile
	$(CC) $(CFLAGS) $(DEFINES) $(SRCS) $(INCLUDES) -o $(PROGRAM)
