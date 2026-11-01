NAME = metaprofiler
SOURCES = $(wildcard *.cpp)

CXX = g++
CXXFLAGS = -fPIC -O2 -m32 -std=c++17 -I. -DMETA_NO_HLSDK -DHAVE_STDINT_H -DNDEBUG
LDFLAGS = -shared -m32 -static-libstdc++ -static-libgcc

all:
	$(CXX) $(CXXFLAGS) -o $(NAME)_amxx_i386.so $(SOURCES) $(LDFLAGS)

clean:
	rm -f *.so *.o
