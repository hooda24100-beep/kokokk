.POSIX:
CXX      = g++
CXXFLAGS = -Wall -Wextra -O3 -std=c++17 \
           -Iimgui -Iimgui/backends -Isrc \
           -mwindows -mavx2 -MMD -MP
LDFLAGS  = -ld3d11 -ldxgi -lgdi32 -luser32 -ldwmapi -ld3dcompiler \
           -static-libgcc -static-libstdc++
TARGET   = colorbot_v7.exe

SRC = main.cpp \
      src/config/Config.cpp \
      src/core/CaptureDXGI.cpp \
      src/core/Scanner.cpp \
      src/input/InputDispatcher.cpp \
      src/ui/Menu.cpp \
      src/utils/ThreadPool.cpp \
      imgui/imgui.cpp \
      imgui/imgui_draw.cpp \
      imgui/imgui_tables.cpp \
      imgui/imgui_widgets.cpp \
      imgui/backends/imgui_impl_win32.cpp \
      imgui/backends/imgui_impl_dx11.cpp

OBJ = $(SRC:.cpp=.o)
DEP = $(OBJ:.o=.d)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) $(OBJ) -o $@ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJ) $(DEP)

-include $(DEP)
