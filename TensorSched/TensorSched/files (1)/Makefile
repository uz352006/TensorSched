CXX = g++
CXXFLAGS = -std=c++17 $(shell sdl2-config --cflags)
LDFLAGS = $(shell sdl2-config --libs) -lSDL2_ttf -lpthread

all: tensorsched

tensorsched: main.cpp scheduler.cpp
	$(CXX) $(CXXFLAGS) main.cpp scheduler.cpp $(LDFLAGS) -o tensorsched

clean:
	rm -f tensorsched

.PHONY: all clean