# 赋勤 EMS_VSG 采集（JSON 配置驱动，只写 MySQL；历史上传见 EMS_VSG_history_uploader）
CXX      := aarch64-rockchip-linux-gnu-g++
CXXFLAGS := -std=c++17
LDFLAGS  := -lpthread -lmodbus

SYSROOT  := /opt/aarch64
MYSQL    := /home/a64/baol/mysql_arm_install/mysql_arm_install/usr/local/mysql
MODBUS   := /home/modbus/install

INCLUDES := \
	-I. \
	-Icore \
	-Idb \
	-Idevices \
	-Iinclude/common \
	-Ithird_party \
	-I$(SYSROOT)/usr/include \
	-I$(MYSQL)/include \
	-I$(MODBUS)/include/modbus

LIBS     := \
	-L$(SYSROOT)/usr/lib \
	-L$(MYSQL)/lib \
	-L$(MODBUS)/lib \
	-lmysqlclient

SRCS := \
	devices/Agc.cpp \
	devices/AMC_Meter.cpp \
	devices/Dido.cpp \
	devices/PCS_Smarten.cpp \
	devices/Bamu.cpp \
	devices/SunPv.cpp \
	core/ModbusRtu.cpp \
	core/ModbusConfigLoader.cpp \
	core/ModbusPollEngine.cpp \
	db/MySQLDB_1.cpp \
	db/logger.cpp \
	main.cpp

TARGET := collect

.PHONY: arm clean

arm: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(LIBS) $(SRCS) $(LDFLAGS) -o $@

clean:
	rm -f $(TARGET)
