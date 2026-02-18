CXX      = g++
CXXFLAGS = -std=c++14 -O2 -g -I.. -I/usr/include/opencv4 -I/usr/include/jsoncpp
LDFLAGS  =
LIBS     = ../filter-generator/libfilter-generator.a \
           -lopencv_videoio -lopencv_imgproc -lopencv_imgcodecs -lopencv_core \
           -ljsoncpp

TARGET   = detect-moving
SRC      = detect-moving.cpp
OBJ      = $(SRC:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean

