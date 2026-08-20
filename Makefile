CXX      = g++
CXXFLAGS = -std=c++17 -O2 -pthread -Wall -Wextra
LDFLAGS  = -lncurses

SRC = src/main.cpp \
	src/editor.cpp \
	src/network.cpp \
	src/crdt.cpp \
	src/ui.cpp \
	src/thread_manager.cpp \
	src/utils.cpp

OBJ    = $(SRC:.cpp=.o)
TARGET = editor

TEST_SRC = tests/crdt_test.cpp \
	src/crdt.cpp \
	src/utils.cpp
TEST_OBJ    = $(TEST_SRC:.cpp=.o)
TEST_TARGET = tests/crdt_test

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ) $(LDFLAGS)

$(TEST_TARGET): $(TEST_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(TEST_OBJ)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

test: $(TEST_TARGET)
	./$(TEST_TARGET)

clean:
	rm -f $(OBJ) $(TARGET) $(TEST_OBJ) $(TEST_TARGET)
