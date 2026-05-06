CXX = g++
CXXFLAGS = -std=c++11 -pthread -O2
TARGET = scheduler_os

all: $(TARGET)

$(TARGET): main.cpp
	$(CXX) $(CXXFLAGS) main.cpp -o $(TARGET)

clean:
	rm -f $(TARGET)
