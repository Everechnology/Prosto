# ProstoCode DLL Build Configuration
# 使用方法: mingw32-make

# 编译器设置
CXX = g++
CC = gcc

# C++ 标准（根据项目规范使用 C++17）
CPP_STANDARD = -std=c++17

# 编译选项
CXXFLAGS = $(CPP_STANDARD) -fpermissive -DPROSTO_BUILD_DLL -D_WIN32 -DWIN32_LEAN_AND_MEAN -fPIC -O2 -Wall -MMD -MP
CFLAGS = -DPROSTO_BUILD_DLL -fPIC -O2 -Wall -MMD -MP

# 包含路径
INCLUDES = -I./include -I./sec -I./src

# 库路径（如果需要）
LIBDIRS = 

# 链接库
LIBS = -lws2_32 -lwinhttp -lbcrypt -lnettle -lhogweed -lnettle -lgmp -lz -lsqlite3 -lcurl -lssl -lcrypto -lzip -lole32 -loleaut32 -luuid

# 源文件目录
SRC_DIR = src
SEC_DIR = sec
INCLUDE_DIR = include

# 输出目录
OUT_DIR = bin
DIST_DIR = dist

# 创建输出目录
$(shell mkdir -p $(OUT_DIR) 2>nul || cmd /c "mkdir $(OUT_DIR)" 2>nul)

# 源文件列表
SRC_CPP_SOURCES = $(wildcard $(SRC_DIR)/*.cpp)
CPP_SOURCES = $(filter-out $(SRC_DIR)/main.cpp, $(SRC_CPP_SOURCES))
C_SOURCES = $(filter-out $(SRC_DIR)/miniz.c, $(wildcard $(SRC_DIR)/*.c))

# 对象文件列表
CPP_OBJECTS = $(addprefix $(OUT_DIR)/, $(notdir $(CPP_SOURCES:.cpp=.o)))
C_OBJECTS = $(addprefix $(OUT_DIR)/, $(notdir $(C_SOURCES:.c=.o)))
DEPS = $(CPP_OBJECTS:.o=.d) $(C_OBJECTS:.o=.d)

# DLL 名称
DLL_NAME = libprosto.dll
STATIC_LIB_NAME = libprosto.a
IMPORT_LIB_NAME = libprosto.dll.a

# 默认目标
all: $(OUT_DIR)/$(DLL_NAME) $(OUT_DIR)/$(STATIC_LIB_NAME)

# 编译 DLL
$(OUT_DIR)/$(DLL_NAME): $(CPP_OBJECTS) $(C_OBJECTS)
	$(CXX) -shared -o $@ $^ $(LIBDIRS) $(LIBS)
	@echo "DLL created: $@"

# 编译静态库
$(OUT_DIR)/$(STATIC_LIB_NAME): $(CPP_OBJECTS) $(C_OBJECTS)
	ar rcs $@ $^
	@echo "Static library created: $@"

# 编译 C++ 源文件
$(OUT_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OUT_DIR)/%.o: $(SEC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# 编译 C 源文件
$(OUT_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# 单独编译 main.cpp 为可执行文件（如果需要）
exe: $(OUT_DIR)/$(DLL_NAME)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $(OUT_DIR)/prostop.exe $(SRC_DIR)/main.cpp $(CPP_OBJECTS) $(C_OBJECTS) $(LIBS)

# dist: 准备发布目录，复制可执行和 DLL 以及必要的 MinGW 运行时 DLL
dist: exe
	$(shell mkdir -p $(DIST_DIR) 2>nul || cmd /c "mkdir $(DIST_DIR)" 2>nul)
	# 复制主程序和库
	cp $(OUT_DIR)/prostop.exe $(DIST_DIR)/ 2>nul || cmd /c "copy $(OUT_DIR)\\prostop.exe $(DIST_DIR)\\"
	cp $(OUT_DIR)/$(DLL_NAME) $(DIST_DIR)/ 2>nul || cmd /c "copy $(OUT_DIR)\\$(DLL_NAME) $(DIST_DIR)\\"
	# 常见的 MinGW 运行时 DLL（按需复制）
	# 优先从 MSYS2 mingw64/bin 复制，若不存在则尝试 cmd copy（Windows 路径）
	cp /c/msys64/mingw64/bin/libgcc_s_seh-1.dll $(DIST_DIR)/ 2>nul || cmd /c "if exist C:\\msys64\\mingw64\\bin\\libgcc_s_seh-1.dll copy C:\\msys64\\mingw64\\bin\\libgcc_s_seh-1.dll $(DIST_DIR)\\"
	cp /c/msys64/mingw64/bin/libstdc++-6.dll $(DIST_DIR)/ 2>nul || cmd /c "if exist C:\\msys64\\mingw64\\bin\\libstdc++-6.dll copy C:\\msys64\\mingw64\\bin\\libstdc++-6.dll $(DIST_DIR)\\"
	cp /c/msys64/mingw64/bin/libwinpthread-1.dll $(DIST_DIR)/ 2>nul || cmd /c "if exist C:\\msys64\\mingw64\\bin\\libwinpthread-1.dll copy C:\\msys64\\mingw64\\bin\\libwinpthread-1.dll $(DIST_DIR)\\"
	@echo "Dist prepared in $(DIST_DIR)/"

# 清理目标
clean:
	del /Q $(OUT_DIR)\*.o $(OUT_DIR)\*.d $(OUT_DIR)\*.dll $(OUT_DIR)\*.a 2>nul || rm -f $(OUT_DIR)/*.o $(OUT_DIR)/*.d $(OUT_DIR)/*.dll $(OUT_DIR)/*.a
	@echo "Build artifacts cleaned"

# 重新构建
rebuild: clean all

.PHONY: all clean exe rebuild

-include $(DEPS)