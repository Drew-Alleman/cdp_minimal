CXX       := g++
CXXFLAGS  := -std=c++20 -Wall -Wextra -O2
INCLUDES  := -I include -I src

SRCS := src/main.cpp \
        src/cdp/browser.cpp \
        src/cdp/cookie.cpp \
        src/cdp/page.cpp \
        src/cdp/result.cpp \
        src/detail/channel.cpp \
        src/detail/connection.cpp

TARGET    := cdp_test

LIBS      := -lboost_thread -lpthread

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
