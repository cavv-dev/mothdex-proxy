CXX      = g++

MINIMAL_FLAGS = -DTORRENT_DISABLE_SUPERSEEDING -DTORRENT_DISABLE_MUTABLE_TORRENTS

LIBTORRENT_CFLAGS = $(shell pkg-config --cflags libtorrent-rasterbar)

CXXFLAGS = -Wall -Wextra -std=c++17 -O2 -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE $(MINIMAL_FLAGS) $(LIBTORRENT_CFLAGS) -I/usr/local/include -I/usr/include/jsoncpp

LDFLAGS  = -L/usr/local/lib -pie -Wl,-z,relro,-z,now \
           -Wl,-Bstatic \
           -ltorrent-rasterbar \
           -ljsoncpp -lssl -lcrypto -lzstd -lz \
           -Wl,-Bdynamic \
           -lpthread -ldl -lm -lc \
           -static-libgcc -static-libstdc++

SRC_DIR  = src
OBJ_DIR  = obj

SRCS     = $(wildcard $(SRC_DIR)/*.cpp)
OBJS     = $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR)/%.o, $(SRCS))

TARGET   = mothdex-proxy

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

clean:
	rm -rf $(OBJ_DIR) $(TARGET)

.PHONY: all clean
