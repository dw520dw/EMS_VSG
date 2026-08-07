# 赋勤充电柜 采集程序
# 目录结构：
#   src/main.cpp            程序入口
#   src/common/             公共库实现（ModbusRtu / MySQLDB_1 / influxDB / logger）
#   src/devices/            设备驱动实现（AMC_Meter / Agc / Bamu / Dido / PCS_Smarten / SunPv）+ 字段映射 .inc
#   include/common/         公共库头文件（含 Config.h）
#   include/devices/        设备驱动头文件

CXX      := aarch64-rockchip-linux-gnu-g++
CXXFLAGS := -std=c++17
LDFLAGS  := -lpthread -lcurl -lmodbus

SYSROOT  := /opt/aarch64
MYSQL    := /home/a64/baol/mysql_arm_install/mysql_arm_install/usr/local/mysql
CURL     := /home/a64/libcurl/libcurl
MODBUS   := /home/modbus/install

INCLUDES := \
	-I$(SYSROOT)/usr/include \
	-I$(MYSQL)/include \
	-I$(CURL)/include \
	-I$(MODBUS)/include/modbus \
	-Isrc \
	-Iinclude \
	-Iinclude/common \
	-Iinclude/devices

LIBS     := \
	-L$(SYSROOT)/usr/lib \
	-L$(MYSQL)/lib \
	-L$(CURL)/lib \
	-L$(MODBUS)/lib \
	-lmysqlclient

SRCS := \
	src/devices/Bamu.cpp \
	src/devices/Agc.cpp \
	src/devices/PCS_Smarten.cpp \
	src/common/ModbusRtu.cpp \
	src/common/MySQLDB_1.cpp \
	src/common/logger.cpp \
	src/common/influxDB.cpp \
	src/devices/AMC_Meter.cpp \
	src/devices/SunPv.cpp \
	src/devices/Dido.cpp \
	src/main.cpp

TARGET := collect

.PHONY: arm clean

arm: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(LIBS) $(SRCS) $(LDFLAGS) -o $@

clean:
	rm -f $(TARGET)
