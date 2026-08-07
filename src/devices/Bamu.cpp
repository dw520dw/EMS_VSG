#include "Bamu.h"
#include "Config.h"

Bamu::Bamu(const char *device_ip, int device_port, int device_address)
    : modbusclient(device_ip, device_port, device_address)
{
}

Bamu::~Bamu()
{
    modbusclient.disconnect();
}

namespace {

struct AirFieldMapEntry {
    int dbAddr;
    const char* influxKey;
    double (*getter)(const Air&);
};

#define AIR_FIELD_BOOL(addr, key, field) \
    { addr, key, [](const Air& a) -> double { return a.airAlarm.field ? 1.0 : 0.0; } }
#define AIR_FIELD_RAW(addr, key, field) \
    { addr, key, [](const Air& a) -> double { return static_cast<double>(a.airData.field); } }
#define AIR_FIELD_R1(addr, key, field) \
    { addr, key, [](const Air& a) -> double { return static_cast<double>(a.airData.field) * BamuRatio_Value; } }

static const AirFieldMapEntry kAirFieldMaps[] = {
#include "Bamu_air_field_maps.inc"
};

static constexpr size_t kAirFieldMapCount = sizeof(kAirFieldMaps) / sizeof(AirFieldMapEntry);
static_assert(kAirFieldMapCount == 109, "Air field map count mismatch");

struct BamuFieldMapEntry {
    int dbAddr;
    const char* influxKey;
    double (*getter)(const BAMUData&, const BAMUAlarm&);
};

#define BAMU_DATA_RAW(addr, key, field) \
    { addr, key, [](const BAMUData& d, const BAMUAlarm&) -> double { return static_cast<double>(d.field); } }
#define BAMU_DATA_R1(addr, key, field) \
    { addr, key, [](const BAMUData& d, const BAMUAlarm&) -> double { return static_cast<double>(d.field) * BamuRatio_Value; } }
#define BAMU_DATA_R3(addr, key, field) \
    { addr, key, [](const BAMUData& d, const BAMUAlarm&) -> double { return static_cast<double>(d.field) * BamuRatio_Value3; } }
#define BAMU_DATA_TEMP(addr, key, field) \
    { addr, key, [](const BAMUData& d, const BAMUAlarm&) -> double { return static_cast<double>(d.field) - 40.0; } }
#define BAMU_ALARM(addr, key, field) \
    { addr, key, [](const BAMUData&, const BAMUAlarm& a) -> double { return a.field ? 1.0 : 0.0; } }

static const BamuFieldMapEntry kBamuFieldMaps[] = {
#include "Bamu_bamu_field_maps.inc"
};

static constexpr size_t kBamuFieldMapCount = sizeof(kBamuFieldMaps) / sizeof(BamuFieldMapEntry);
static_assert(kBamuFieldMapCount == 96, "BAMU field map count mismatch");

struct BmsFieldMapEntry {
    int dbAddr;
    const char* influxKey;
    double (*getter)(const BMSData&, const BMSAlarm&);
};

#define BMS_DATA_RAW(addr, key, field) \
    { addr, key, [](const BMSData& d, const BMSAlarm&) -> double { return static_cast<double>(d.field); } }
#define BMS_DATA_R1(addr, key, field) \
    { addr, key, [](const BMSData& d, const BMSAlarm&) -> double { return static_cast<double>(d.field) * BamuRatio_Value; } }
#define BMS_DATA_R1_BIAS(addr, key, field, bias) \
    { addr, key, [](const BMSData& d, const BMSAlarm&) -> double { return static_cast<double>(d.field) * BamuRatio_Value - static_cast<double>(bias); } }
#define BMS_DATA_R3(addr, key, field) \
    { addr, key, [](const BMSData& d, const BMSAlarm&) -> double { return static_cast<double>(d.field) * BamuRatio_Value3; } }
#define BMS_DATA_TEMP(addr, key, field) \
    { addr, key, [](const BMSData& d, const BMSAlarm&) -> double { return static_cast<double>(d.field) - 40.0; } }
#define BMS_ALARM(addr, key, field) \
    { addr, key, [](const BMSData&, const BMSAlarm& a) -> double { return a.field ? 1.0 : 0.0; } }

static const BmsFieldMapEntry kBmsFieldMaps[] = {
#include "Bamu_bms_field_maps.inc"
};

static constexpr size_t kBmsFieldMapCount = sizeof(kBmsFieldMaps) / sizeof(BmsFieldMapEntry);
static_assert(kBmsFieldMapCount == 76, "BMS field map count mismatch");

} // namespace

void Bamu::readBamuAlarm()
{
    if (modbusclient.readInputBits(BAMU_ALARM_START_ADDR, BAMU_ALARM_COUNT, arr_input_bits) != -1)
    {
        // 使用辅助函数简化位赋值（Bit 1-44连续）
        setBitFromArray(bamuAlarm.mBankVolLower_Lv1, arr_input_bits, 0);
        setBitFromArray(bamuAlarm.mBankVolLower_Lv2, arr_input_bits, 1);
        setBitFromArray(bamuAlarm.mBankVolLower_Lv3, arr_input_bits, 2);
        setBitFromArray(bamuAlarm.mBankVolUpper_Lv1, arr_input_bits, 3);
        setBitFromArray(bamuAlarm.mBankVolUpper_Lv2, arr_input_bits, 4);
        setBitFromArray(bamuAlarm.mBankVolUpper_Lv3, arr_input_bits, 5);
        setBitFromArray(bamuAlarm.mBankCurrentUpper_Lv1, arr_input_bits, 6);
        setBitFromArray(bamuAlarm.mBankCurrentUpper_Lv2, arr_input_bits, 7);
        setBitFromArray(bamuAlarm.mBankCurrentUpper_Lv3, arr_input_bits, 8);
        setBitFromArray(bamuAlarm.mmInsLower_Lv1, arr_input_bits, 9);
        setBitFromArray(bamuAlarm.mmInsLower_Lv2, arr_input_bits, 10);
        setBitFromArray(bamuAlarm.mmInsLower_Lv3, arr_input_bits, 11);
        setBitFromArray(bamuAlarm.mModelTempLower_Lv1, arr_input_bits, 12);
        setBitFromArray(bamuAlarm.mModelTempLower_Lv2, arr_input_bits, 13);
        setBitFromArray(bamuAlarm.mModelTempLower_Lv3, arr_input_bits, 14);
        setBitFromArray(bamuAlarm.mModelTempUpper_Lv1, arr_input_bits, 15);
        setBitFromArray(bamuAlarm.mModelTempUpper_Lv2, arr_input_bits, 16);
        setBitFromArray(bamuAlarm.mModelTempUpper_Lv3, arr_input_bits, 17);
        setBitFromArray(bamuAlarm.mCellVolUpper_Lv1, arr_input_bits, 18);
        setBitFromArray(bamuAlarm.mCellVolUpper_Lv2, arr_input_bits, 19);
        setBitFromArray(bamuAlarm.mCellVolUpper_Lv3, arr_input_bits, 20);
        setBitFromArray(bamuAlarm.mCellVolLower_Lv1, arr_input_bits, 21);
        setBitFromArray(bamuAlarm.mCellVolLower_Lv2, arr_input_bits, 22);
        setBitFromArray(bamuAlarm.mCellVolLower_Lv3, arr_input_bits, 23);
        setBitFromArray(bamuAlarm.mCellVolDif_Lv1, arr_input_bits, 24);
        setBitFromArray(bamuAlarm.mCellVolDif_Lv2, arr_input_bits, 25);
        setBitFromArray(bamuAlarm.mCellVolDif_Lv3, arr_input_bits, 26);
        setBitFromArray(bamuAlarm.mCellTempLower_Lv1, arr_input_bits, 27);
        setBitFromArray(bamuAlarm.mCellTempLower_Lv2, arr_input_bits, 28);
        setBitFromArray(bamuAlarm.mCellTempLower_Lv3, arr_input_bits, 29);
        setBitFromArray(bamuAlarm.mCellTempUpper_Lv1, arr_input_bits, 30);
        setBitFromArray(bamuAlarm.mCellTempUpper_Lv2, arr_input_bits, 31);
        setBitFromArray(bamuAlarm.mCellTempUpper_Lv3, arr_input_bits, 32);
        setBitFromArray(bamuAlarm.mCellTempDif_Lv1, arr_input_bits, 33);
        setBitFromArray(bamuAlarm.mCellTempDif_Lv2, arr_input_bits, 34);
        setBitFromArray(bamuAlarm.mCellTempDif_Lv3, arr_input_bits, 35);
        setBitFromArray(bamuAlarm.SOCLower_Lv1, arr_input_bits, 36);
        setBitFromArray(bamuAlarm.SOCLower_Lv2, arr_input_bits, 37);
        setBitFromArray(bamuAlarm.SOCLower_Lv3, arr_input_bits, 38);
        setBitFromArray(bamuAlarm.SOCUpper_Lv1, arr_input_bits, 39);
        setBitFromArray(bamuAlarm.SOCUpper_Lv2, arr_input_bits, 40);
        setBitFromArray(bamuAlarm.SOCUpper_Lv3, arr_input_bits, 41);
        setBitFromArray(bamuAlarm.SOHLower_Lv1, arr_input_bits, 42);
        setBitFromArray(bamuAlarm.SOHLower_Lv2, arr_input_bits, 43);
        setBitFromArray(bamuAlarm.SOHLower_Lv3, arr_input_bits, 44);

        // Bit 48-56（不连续）
        setBitFromArray(bamuAlarm.MainCtrl_Disconnected, arr_input_bits, 48);
        setBitFromArray(bamuAlarm.SBC_Disconnected, arr_input_bits, 49);
        setBitFromArray(bamuAlarm.Voltage_Abnormal_Alarm, arr_input_bits, 50);
        setBitFromArray(bamuAlarm.Contactor_Break_Alarm, arr_input_bits, 51);
        setBitFromArray(bamuAlarm.Contactor_Close_Alarm, arr_input_bits, 52);
        setBitFromArray(bamuAlarm.Charging_stop_Alarm, arr_input_bits, 53);
        setBitFromArray(bamuAlarm.Discharging_stop_Alarm, arr_input_bits, 54);
        setBitFromArray(bamuAlarm.BMS_Alarm_Total, arr_input_bits, 55);
        setBitFromArray(bamuAlarm.BMS_Fault_Total, arr_input_bits, 56);

        // Bit 69-79（不连续）
        setBitFromArray(bamuAlarm.mBankTempUpper_Lv1, arr_input_bits, 69);
        setBitFromArray(bamuAlarm.mBankTempUpper_Lv2, arr_input_bits, 70);
        setBitFromArray(bamuAlarm.mBankTempUpper_Lv3, arr_input_bits, 71);
        setBitFromArray(bamuAlarm.mModelVolUpper_Lv1, arr_input_bits, 72);
        setBitFromArray(bamuAlarm.mModelVolUpper_Lv2, arr_input_bits, 73);
        setBitFromArray(bamuAlarm.mModelVolUpper_Lv3, arr_input_bits, 74);
        setBitFromArray(bamuAlarm.mModelVolLower_Lv1, arr_input_bits, 75);
        setBitFromArray(bamuAlarm.mModelVolLower_Lv2, arr_input_bits, 76);
        setBitFromArray(bamuAlarm.mModelVolLower_Lv3, arr_input_bits, 77);
        setBitFromArray(bamuAlarm.mCellVolAcqFault, arr_input_bits, 78);
        setBitFromArray(bamuAlarm.mCellTempAcqFault, arr_input_bits, 79);
    }
}

void Bamu::write_Bamu_data(MySQLDatabase& db){
    dbList.clearData();
    for (const auto& field : kBamuFieldMaps) {
        dbList.addData(field.dbAddr, field.getter(bamuData, bamuAlarm));
    }
    db.insert(dbList.spliceData(TableNameBamu));

    bool sysStopFlag = db.select(112, "data_total");
    if (sysStopFlag)
    {
        modbusclient.writeRegister(503,2,TableNameBamu);
    }

}

void Bamu::alarmLevel()
{
    // 1级报警 - 2级报警 - 3级报警
    if (bamuAlarm.mModelVolLower_Lv1 || bamuAlarm.mModelVolUpper_Lv1 || bamuAlarm.mModelTempUpper_Lv1 ||
        bamuAlarm.mModelTempLower_Lv1 || bamuAlarm.mCellVolUpper_Lv1 || bamuAlarm.mCellVolLower_Lv1 ||
        bamuAlarm.mCellTempUpper_Lv1 || bamuAlarm.mCellTempLower_Lv1 || bamuAlarm.SOCLower_Lv1 ||
        bamuAlarm.SOCUpper_Lv1 || bamuAlarm.SOHLower_Lv1 ||
        bamuAlarm.mBankTempUpper_Lv1 || bamuAlarm.mBankVolLower_Lv1 || bamuAlarm.mBankVolUpper_Lv1 ||
        bamuAlarm.mBankCurrentUpper_Lv1 || bamuAlarm.mmInsLower_Lv1 || bamuAlarm.mCellVolDif_Lv1 ||
        bamuAlarm.mCellTempDif_Lv1)
    {
        bamuData.alarmlevel = 1;
    }
    else if (bamuAlarm.mModelVolLower_Lv2 || bamuAlarm.mModelVolUpper_Lv2 || bamuAlarm.mModelTempUpper_Lv2 ||
             bamuAlarm.mModelTempLower_Lv2 || bamuAlarm.mCellVolUpper_Lv2 || bamuAlarm.mCellVolLower_Lv2 ||
             bamuAlarm.mCellTempUpper_Lv2 || bamuAlarm.mCellTempLower_Lv2 || bamuAlarm.SOCLower_Lv2 ||
             bamuAlarm.SOCUpper_Lv2 || bamuAlarm.SOHLower_Lv2 ||
             bamuAlarm.mBankTempUpper_Lv2 || bamuAlarm.mBankVolLower_Lv2 || bamuAlarm.mBankVolUpper_Lv2 ||
             bamuAlarm.mBankCurrentUpper_Lv2 || bamuAlarm.mmInsLower_Lv2 || bamuAlarm.mCellVolDif_Lv2 ||
             bamuAlarm.mCellTempDif_Lv2 || bamuAlarm.Charging_stop_Alarm || bamuAlarm.Discharging_stop_Alarm)
    {
        bamuData.alarmlevel = 2;
    }
    else if (bamuAlarm.mModelVolLower_Lv3 || bamuAlarm.mModelVolUpper_Lv3 || bamuAlarm.mModelTempUpper_Lv3 ||
             bamuAlarm.mModelTempLower_Lv3 || bamuAlarm.mCellVolUpper_Lv3 || bamuAlarm.mCellVolLower_Lv3 ||
             bamuAlarm.mCellTempUpper_Lv3 || bamuAlarm.mCellTempLower_Lv3 || bamuAlarm.SOCLower_Lv3 ||
             bamuAlarm.SOCUpper_Lv3 || bamuAlarm.SOHLower_Lv3 ||
             bamuAlarm.mBankTempUpper_Lv3 || bamuAlarm.mBankVolLower_Lv3 || bamuAlarm.mBankVolUpper_Lv3 ||
             bamuAlarm.mBankCurrentUpper_Lv3 || bamuAlarm.mmInsLower_Lv3 || bamuAlarm.mCellVolDif_Lv3 ||
             bamuAlarm.mCellTempDif_Lv3 || bamuAlarm.SBC_Disconnected || bamuAlarm.MainCtrl_Disconnected ||
             bamuAlarm.Voltage_Abnormal_Alarm || bamuAlarm.Contactor_Break_Alarm || bamuAlarm.Contactor_Close_Alarm ||
             bamuAlarm.mCellVolAcqFault ||bamuAlarm.mCellTempAcqFault || bamuAlarm.BMS_Fault_Total)
    {
        bamuData.alarmlevel = 3;
    }else{
	bamuData.alarmlevel = 0;
  }
}


void Bamu::readBamuData()
{
    if (modbusclient.readInputRegisters(BAMU_DATA_START_ADDR, BAMU_DATA_COUNT, arr_input_registers) != -1)
    {
        bamuData.Voltage = arr_input_registers[0];
        bamuData.Current = static_cast<int16_t>(arr_input_registers[1] * BamuRatio_Value - 1600);
        bamuData.SOC = arr_input_registers[2];
        bamuData.SOH = arr_input_registers[3];
        bamuData.Max_Cell_Voltage = arr_input_registers[4];
        bamuData.Max_Cell_Voltage_group = arr_input_registers[5];
        bamuData.Max_Cell_Voltage_Number = arr_input_registers[6];
        bamuData.Min_Cell_Voltage = arr_input_registers[7];
        bamuData.Min_Cell_Voltage_group = arr_input_registers[8];
        bamuData.Min_Cell_Voltage_Number = arr_input_registers[9];
        bamuData.Max_Cell_Temperature = arr_input_registers[10];
        bamuData.Max_Cell_Temperature_group = arr_input_registers[11];
        bamuData.Max_Cell_Temperature_Number = arr_input_registers[12];
        bamuData.Min_Cell_Temperature = arr_input_registers[13];
        bamuData.Min_Cell_Temperature_group = arr_input_registers[14];
        bamuData.Min_Cell_Temperature_Number = arr_input_registers[15];
        bamuData.Charging_Capacity = (static_cast<uint32_t>(arr_input_registers[16]) << 16) | arr_input_registers[17];
        bamuData.Discharging_Capacity = (static_cast<uint32_t>(arr_input_registers[18]) << 16) | arr_input_registers[19];
        bamuData.Single_Charging_Capacity = (static_cast<uint32_t>(arr_input_registers[20]) << 16) | arr_input_registers[21];
        bamuData.Single_Discharging_Capacity = (static_cast<uint32_t>(arr_input_registers[22]) << 16) | arr_input_registers[23];
        bamuData.Allow_Max_Charging_Power = arr_input_registers[31];
        bamuData.Allow_Max_Discharging_Power = arr_input_registers[30];
        bamuData.Allow_Max_Charging_Current = arr_input_registers[33];
        bamuData.Allow_Max_Discharging_Current = arr_input_registers[32];
        bamuData.Day_Charging_Capacity = (static_cast<uint32_t>(arr_input_registers[36]) << 16) | arr_input_registers[37];
        bamuData.Day_Discharging_Capacity = (static_cast<uint32_t>(arr_input_registers[38]) << 16) | arr_input_registers[39];
        bamuData.Run_Temperture = (arr_input_registers[40] - 40);
        bamuData.State = arr_input_registers[41];
        bamuData.Insulation_Resistance = arr_input_registers[43];
        bamuData.Power = static_cast<int32_t>(bamuData.Voltage * BamuRatio_Value * bamuData.Current);
    } 
}


void Bamu::write_logic_data(MySQLDatabase& db)
{
    dbList.clearData();
    // logic 表：液冷机组通讯故障 addr 201/1201/2201…（每台空调 1 点，两簇共用一台）
    const int clusterCount = clampBmsClusterCount(batteryNumber);
    const int airUnitCount = bmsClusterToAirUnitId(clusterCount);
    for (int airUnitId = 1; airUnitId <= airUnitCount; ++airUnitId) {
        if (!isValidAirUnitId(airUnitId)) {
            break;
        }
        dbList.addData(airUnitToLogicCommAddr(airUnitId),
                       static_cast<int>(air[airUnitId].airAlarm.comm_failure));
    }
    dbList.addData(401, bamuData.commFlag);
    dbList.addData(402, bamuData.SOC);
    dbList.addData(403, bamuData.Allow_Max_Charging_Power * BamuRatio_Value);
    dbList.addData(404, bamuData.Allow_Max_Discharging_Power * BamuRatio_Value);
    dbList.addData(405, bamuData.alarmlevel);
    dbList.addData(406, static_cast<int>(diSignals.EPO));
    dbList.addData(412, static_cast<int>(diSignals.FireLevel1));
    dbList.addData(413, static_cast<int>(diSignals.FireControl));
    dbList.addData(10, static_cast<int>(diSignals.TotalSwitch));
    db.insert(dbList.spliceData(tableNameLogic));

    dbList.clearData();
    dbList.addData(4, static_cast<int>(diSignals.EPO));
    dbList.addData(5, static_cast<int>(diSignals.FireLevel1));
    dbList.addData(6, static_cast<int>(diSignals.FireControl));
    dbList.addData(7, static_cast<int>(diSignals.TotalSwitch));
    db.insert(dbList.spliceData("dodi"));
}

void Bamu::readAirData(int bmsClusterId){
    if (!isValidBmsClusterId(bmsClusterId) || !isAirUnitLeadCluster(bmsClusterId)) {
        return;
    }//防止非法簇数
    const int airUnitId = bmsClusterToAirUnitId(bmsClusterId);
    if (!isValidAirUnitId(airUnitId)) {
        return;
    }//判断是否读取空调
    const int dataOffset = (airUnitId - 1) *100;
    const int alarmOffset = (airUnitId - 1) *200;
    Air& a = air[airUnitId];
    if (modbusclient.readInputRegisters(AIR_DATA_START_ADDR + dataOffset, AIR_DATA_COUNT, arr_input_registers) != -1) {
        const uint16_t* r = arr_input_registers;
        a.airData.outlet_water_temperature = r[0];   // 60100
        a.airData.return_water_temperature = r[1];     // 60101
        a.airData.outlet_water_pressure = r[2];        // 60102
        a.airData.return_water_pressure = r[3];        // 60103
        a.airData.ambient_temperature = r[4];          // 60104
        a.airData.water_pump_speed = r[5];             // 60105
        a.airData.compressor_speed = r[6];             // 60106
        a.airData.fan_speed = r[15];                   // 60115
        a.airData.inlet_temperature = r[16];           // 60116
        a.airData.exhaust_temperature = r[17];         // 60117
        a.airData.cooling_setpoint = r[31];            // 60131
        a.airData.cooling_hysteresis = r[32];          // 60132
        a.airData.heating_setpoint = r[33];            // 60133
        a.airData.heating_hysteresis = r[34];          // 60134
        a.airData.unit_status = r[41];                 // 60141
        a.airData.operation_mode = r[42];              // 60142
    }

    if(a.airData.water_pump_speed > 5){
        a.airData.water_pump_status = 1;
    }else{
        a.airData.water_pump_status = 0;
    }

    if(a.airData.compressor_speed > 5){
        a.airData.compressor_status = 1;
    }else{
        a.airData.compressor_status = 0;
    }

    if(a.airData.fan_speed > 5){
        a.airData.fan_status = 1;
    }else{
        a.airData.fan_status = 0;
    }


    if (modbusclient.readInputBits(AIR_ALARM_START_ADDR + alarmOffset, AIR_ALARM_COUNT, arr_input_bits) != -1) {
#include "Bamu_air_alarm_read.inc"
    }
}

bool Bamu::readDiSignals(DiSignals& outData)
{
    if (modbusclient.readInputBits(DI_START_ADDR, DI_COUNT, arr_input_bits) != -1) {
        setBitFromArray(outData.EPO, arr_input_bits, 1);
        setBitFromArray(outData.TotalSwitch, arr_input_bits, 2);
        setBitFromArray(outData.FireLevel1, arr_input_bits, 4);
        setBitFromArray(outData.FireControl, arr_input_bits, 5);
        return true;
    }
    return false;
}

void Bamu::write_air_data(MySQLDatabase& db, int bmsClusterId){
    if (!isValidBmsClusterId(bmsClusterId)) {
        return;
    }
    const int airUnitId = bmsClusterToAirUnitId(bmsClusterId);
    if (!isValidAirUnitId(airUnitId)) {
        return;
    }
    const Air& a = air[airUnitId];
    dbList.clearData();
    for (const auto& field : kAirFieldMaps) {
        dbList.addData(field.dbAddr, field.getter(a));
    }
    db.insert(dbList.spliceData(TableNameAir + std::to_string(airUnitId)));
    db.update(airUnitToComAlarmAddr(airUnitId),
              static_cast<int>(a.airAlarm.comm_failure), "com_alarm");
}

void Bamu::airHistorydata(int airUnitId){
    if (!isValidAirUnitId(airUnitId)) {
        return;
    }
    const Air& a = air[airUnitId];
    historyDbList.clearData();
    for (const auto& field : kAirFieldMaps) {
        historyDbList.addData(field.influxKey, field.getter(a));
    }
    influxDb.insert(historyDbList.spliceData(TableNameAir + std::to_string(airUnitId)));
}

void Bamu::bamuHistorydata()
{
    historyDbList.clearData();
    for (const auto& field : kBamuFieldMaps) {
        historyDbList.addData(field.influxKey, field.getter(bamuData, bamuAlarm));
    }
    influxDb.insert(historyDbList.spliceData(TableNameBamu));
}

void Bamu::readBmsData(int id)
{
    // 根据id计算地址偏移量: id=1(+0) … id=10(+27000)，步长 3000
    int addressOffset = (id - 1) * 3000;
    int addressAlarmOffset = (id - 1) * 100;
    
    if (modbusclient.readInputRegisters(100 + addressOffset, 31, arr_input_registers) != -1)
    {
        bmsData[id].Battery_State = arr_input_registers[0];
        bmsData[id].Allow_Max_Charging_Power = arr_input_registers[1];
        bmsData[id].Allow_Max_Discharging_Power = arr_input_registers[2];
        bmsData[id].Allow_Max_Charging_Voltage = arr_input_registers[3];
        bmsData[id].Allow_Max_Discharging_Voltage = arr_input_registers[4];
        bmsData[id].Allow_Max_Charging_Current = arr_input_registers[5];
        bmsData[id].Allow_Max_Discharging_Current = arr_input_registers[6];
        bmsData[id].DI1 = arr_input_registers[7];
        bmsData[id].DI2 = arr_input_registers[8];
        bmsData[id].DI3 = arr_input_registers[9];
        bmsData[id].DI4 = arr_input_registers[10];
        bmsData[id].DI5 = arr_input_registers[11];
        bmsData[id].DI6 = arr_input_registers[12];
        bmsData[id].DI7 = arr_input_registers[13];
        bmsData[id].DI8 = arr_input_registers[14];
        bmsData[id].Voltage = arr_input_registers[15];
        bmsData[id].Current = static_cast<int16_t>(arr_input_registers[16]);
        bmsData[id].Module_Temperature = arr_input_registers[17];
        bmsData[id].SOC = arr_input_registers[18];
        bmsData[id].SOH = arr_input_registers[19];
        bmsData[id].Insulation_Resistance = arr_input_registers[20];
        bmsData[id].Ave_Cell_Voltage = arr_input_registers[21];
        bmsData[id].Ave_Cell_Temperature = arr_input_registers[22];
        bmsData[id].Max_Cell_Voltage = arr_input_registers[23];
        bmsData[id].Max_Cell_Voltage_Number = arr_input_registers[24];
        bmsData[id].Min_Cell_Voltage = arr_input_registers[25];
        bmsData[id].Min_Cell_Voltage_Number = arr_input_registers[26];
        bmsData[id].Max_Cell_Temperature = arr_input_registers[27];
        bmsData[id].Max_Cell_Temperature_Number = arr_input_registers[28];
        bmsData[id].Min_Cell_Temperature = arr_input_registers[29];
        bmsData[id].Min_Cell_Temperature_Number = arr_input_registers[30];
        bmsData[id].Power = static_cast<int32_t>(bmsData[id].Voltage * BamuRatio_Value * (bmsData[id].Current * BamuRatio_Value - 1600));
    }

    if (modbusclient.readInputBits(200 + addressAlarmOffset, 37, arr_input_bits) != -1)
    {
        setBitFromArray(bmsAlarm[id].Comm_Error, arr_input_bits, 0);
        setBitFromArray(bmsAlarm[id].mBankVolLower_Lv1, arr_input_bits, 1);
        setBitFromArray(bmsAlarm[id].mBankVolLower_Lv2, arr_input_bits, 2);
        setBitFromArray(bmsAlarm[id].mBankVolLower_Lv3, arr_input_bits, 3);
        setBitFromArray(bmsAlarm[id].mBankVolUpper_Lv1, arr_input_bits, 4);
        setBitFromArray(bmsAlarm[id].mBankVolUpper_Lv2, arr_input_bits, 5);
        setBitFromArray(bmsAlarm[id].mBankVolUpper_Lv3, arr_input_bits, 6);
        setBitFromArray(bmsAlarm[id].mBankCurrentUpper_Lv1, arr_input_bits, 7);
        setBitFromArray(bmsAlarm[id].mBankCurrentUpper_Lv2, arr_input_bits, 8);
        setBitFromArray(bmsAlarm[id].mBankCurrentUpper_Lv3, arr_input_bits, 9);
        setBitFromArray(bmsAlarm[id].mCellVolLower_Lv1, arr_input_bits, 10);
        setBitFromArray(bmsAlarm[id].mCellVolLower_Lv2, arr_input_bits, 11);
        setBitFromArray(bmsAlarm[id].mCellVolLower_Lv3, arr_input_bits, 12);
        setBitFromArray(bmsAlarm[id].mCellVolUpper_Lv1, arr_input_bits, 13);
        setBitFromArray(bmsAlarm[id].mCellVolUpper_Lv2, arr_input_bits, 14);
        setBitFromArray(bmsAlarm[id].mCellVolUpper_Lv3, arr_input_bits, 15);
        setBitFromArray(bmsAlarm[id].mCellTempLower_Lv1, arr_input_bits, 16);
        setBitFromArray(bmsAlarm[id].mCellTempLower_Lv2, arr_input_bits, 17);
        setBitFromArray(bmsAlarm[id].mCellTempLower_Lv3, arr_input_bits, 18);
        setBitFromArray(bmsAlarm[id].mCellTempUpper_Lv1, arr_input_bits, 19);
        setBitFromArray(bmsAlarm[id].mCellTempUpper_Lv2, arr_input_bits, 20);
        setBitFromArray(bmsAlarm[id].mCellTempUpper_Lv3, arr_input_bits, 21);
        setBitFromArray(bmsAlarm[id].SOCLower_Lv1, arr_input_bits, 22);
        setBitFromArray(bmsAlarm[id].SOCLower_Lv2, arr_input_bits, 23);
        setBitFromArray(bmsAlarm[id].SOCLower_Lv3, arr_input_bits, 24);
        setBitFromArray(bmsAlarm[id].SOCUpper_Lv1, arr_input_bits, 25);
        setBitFromArray(bmsAlarm[id].SOCUpper_Lv2, arr_input_bits, 26);
        setBitFromArray(bmsAlarm[id].SOCUpper_Lv3, arr_input_bits, 27);
        setBitFromArray(bmsAlarm[id].SOHLower_Lv1, arr_input_bits, 28);
        setBitFromArray(bmsAlarm[id].SOHLower_Lv2, arr_input_bits, 29);
        setBitFromArray(bmsAlarm[id].SOHLower_Lv3, arr_input_bits, 30);
        setBitFromArray(bmsAlarm[id].mCellVolDif_Lv1, arr_input_bits, 31);
        setBitFromArray(bmsAlarm[id].mCellVolDif_Lv2, arr_input_bits, 32);
        setBitFromArray(bmsAlarm[id].mCellVolDif_Lv3, arr_input_bits, 33);
        setBitFromArray(bmsAlarm[id].mCellTempDif_Lv1, arr_input_bits, 34);
        setBitFromArray(bmsAlarm[id].mCellTempDif_Lv2, arr_input_bits, 35);
        setBitFromArray(bmsAlarm[id].mCellTempDif_Lv3, arr_input_bits, 36);
    }

    if (modbusclient.readInputBits(277 + addressAlarmOffset, 11, arr_input_bits) != -1)
    {
        setBitFromArray(bmsAlarm[id].mModelTempUpper_Lv1, arr_input_bits, 0);
        setBitFromArray(bmsAlarm[id].mModelTempUpper_Lv2, arr_input_bits, 1);
        setBitFromArray(bmsAlarm[id].mModelTempUpper_Lv3, arr_input_bits, 2);
        setBitFromArray(bmsAlarm[id].mModelVolUpper_Lv1, arr_input_bits, 3);
        setBitFromArray(bmsAlarm[id].mModelVolUpper_Lv2, arr_input_bits, 4);
        setBitFromArray(bmsAlarm[id].mModelVolUpper_Lv3, arr_input_bits, 5);
        setBitFromArray(bmsAlarm[id].mModelVolLower_Lv1, arr_input_bits, 6);
        setBitFromArray(bmsAlarm[id].mModelVolLower_Lv2, arr_input_bits, 7);
        setBitFromArray(bmsAlarm[id].mModelVolLower_Lv3, arr_input_bits, 8);
        setBitFromArray(bmsAlarm[id].mCellVolAcqFault, arr_input_bits, 9);
        setBitFromArray(bmsAlarm[id].mCellTempAcqFault, arr_input_bits, 10);
    }
}

void Bamu::readBmsCellData(int id)
{
    if (!isValidBmsClusterId(id)) {
        return;
    }
    const int addressOffset = (id - 1) * 3000;

    if (modbusclient.readInputRegisters(191 + addressOffset, BMS_CELL_MODBUS_BLOCK, arr_input_registers) != -1)
    {
        for (int i = 0; i < BMS_CELL_MODBUS_BLOCK; i++)
        {
            bmsData[id].Cell_Voltage[i] = arr_input_registers[i];
        }
    }

    if (modbusclient.readInputRegisters(311 + addressOffset, BMS_CELL_MODBUS_BLOCK, arr_input_registers) != -1)
    {
        for (int i = 0; i < BMS_CELL_MODBUS_BLOCK; i++)
        {
            bmsData[id].Cell_Voltage[i + BMS_CELL_MODBUS_BLOCK] = arr_input_registers[i];
        }
    }

    if (modbusclient.readInputRegisters(431 + addressOffset, BMS_CELL_MODBUS_BLOCK2, arr_input_registers) != -1)
    {
        const int volThirdBase = BMS_CELL_MODBUS_BLOCK * 2; // 120+120=240
        for (int i = 0; i < BMS_CELL_MODBUS_BLOCK2; i++)
        {
            bmsData[id].Cell_Voltage[volThirdBase + i] = arr_input_registers[i];
        }
    }

    if (modbusclient.readInputRegisters(891 + addressOffset, BMS_CELL_MODBUS_BLOCK, arr_input_registers) != -1)
    {
        for (int i = 0; i < BMS_CELL_MODBUS_BLOCK; i++)
        {
            bmsData[id].Cell_Temperature[i] = arr_input_registers[i] - 40;
        }
    }

    if (modbusclient.readInputRegisters(1011 + addressOffset, BMS_CELL_MODBUS_BLOCK1, arr_input_registers) != -1)
    {
        const int tempSecondBase = BMS_CELL_MODBUS_BLOCK; // 120
        for (int i = 0; i < BMS_CELL_MODBUS_BLOCK1; i++)
        {
            bmsData[id].Cell_Temperature[tempSecondBase + i] = arr_input_registers[i] - 40;
        }
    }
}

void Bamu::write_bms_data(MySQLDatabase& db, int id)
{
    dbList.clearData();
    for (const auto& field : kBmsFieldMaps) {
        dbList.addData(field.dbAddr, field.getter(bmsData[id], bmsAlarm[id]));
    }
    db.insert(dbList.spliceData(TableNameBms + std::to_string(id)));
}

void Bamu::write_cell_data(MySQLDatabase& db, int id)
{
    auto& lastVolValues = lastCellVolDbValues_[static_cast<size_t>(id)];
    auto& lastTempValues = lastCellTempDbValues_[static_cast<size_t>(id)];
    bool& hasVolSnapshot = hasLastCellVolDbSnapshot_[static_cast<size_t>(id)];
    bool& hasTempSnapshot = hasLastCellTempDbSnapshot_[static_cast<size_t>(id)];

    dbList.clearData();
    for (int i = 0; i < BMS_CELL_VOLTAGE_COUNT; i++)
    {
        const int addr = i + 1;
        addDbDataIfChanged(dbList, addr, bmsData[id].Cell_Voltage[i] * BamuRatio_Value3,
                           lastVolValues, hasVolSnapshot);
    }
    hasVolSnapshot = true;
    db.insert(dbList.spliceData(tableNameBMScellvol + std::to_string(id)));

    dbList.clearData();
    for (int i = 0; i < BMS_CELL_TEMPERATURE_COUNT; i++)
    {
        const int addr = i + 1;
        addDbDataIfChanged(dbList, addr, bmsData[id].Cell_Temperature[i],
                           lastTempValues, hasTempSnapshot);
    }
    hasTempSnapshot = true;
    db.insert(dbList.spliceData(tableNameBMScelltemp + std::to_string(id)));
}

void Bamu::write_Cell_historydata(int id) {
    if (!isValidBmsClusterId(id)) {
        return;
    }
    historyDbList.clearData();
    for (int i = 0; i < BMS_CELL_VOLTAGE_COUNT; i++) {
        historyDbList.addData("cellVol_" + std::to_string(i + 1), bmsData[id].Cell_Voltage[i] * BamuRatio_Value3);
    }
    influxDb.insert(historyDbList.spliceData(tableNameBMScellvol + std::to_string(id)));

    historyDbList.clearData();
    for (int i = 0; i < BMS_CELL_TEMPERATURE_COUNT; i++) {
        historyDbList.addData("cellTemp_" + std::to_string(i + 1), bmsData[id].Cell_Temperature[i]);
    }
    influxDb.insert(historyDbList.spliceData(tableNameBMScelltemp + std::to_string(id)));
}

// BMS历史数据上传（数据 + 告警合并写入 bmsN 表）
void Bamu::write_Bmshistorydata(int id) {
    if (!isValidBmsClusterId(id)) {
        return;
    }
    historyDbList.clearData();
    for (const auto& field : kBmsFieldMaps) {
        historyDbList.addData(field.influxKey, field.getter(bmsData[id], bmsAlarm[id]));
    }
    influxDb.insert(historyDbList.spliceData(TableNameBms + std::to_string(id)));
}

void Bamu::processAllBamuOperations(MySQLDatabase& db) {
    try {
        batteryNumber = db.select(708,"qt");

        if (!modbusclient.commTest(1, 1, arr_uint16, TableNameBamu)) {
            bamuData.commErrCount++;
            if (bamuData.commErrCount > 3) {
                bamuData.commFlag = 1;
                db.update(0, bamuData.commFlag, TableNameBamu);
                db.update(1, bamuData.commFlag, "com_alarm");
                return;
            }
        } else {
            bamuData.commErrCount = 0;
            bamuData.commFlag = 0;
        }
        db.update(0, bamuData.commFlag, TableNameBamu);
        db.update(1, bamuData.commFlag, "com_alarm");

        readDiSignals(diSignals);
        readBamuData();
        readBamuAlarm();
        alarmLevel();
        write_Bamu_data(db);
        write_logic_data(db);

        const auto now = std::chrono::steady_clock::now();
        if (lastBamuHistoryTime == std::chrono::steady_clock::time_point{} ||
            now - lastBamuHistoryTime >= std::chrono::seconds(30)) {
            bamuHistorydata();
            lastBamuHistoryTime = now;
        }

    } catch (const std::exception& e) {
        std::cerr << "Error in processAllBamuOperations: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "Unknown error in processAllBamuOperations" << std::endl;
    }
}

void Bamu::processAllBmsOperations(MySQLDatabase& db, int id) {
    if (!isValidBmsClusterId(id)) {
        return;
    }
    try {
        // BMS 与 BAMU 共用同一 Modbus TCP，不再单独 commTest
        bmsData[id].commFlag = bamuData.commFlag;
        db.update(0, bmsData[id].commFlag, TableNameBms + std::to_string(id));

        if (bamuData.commErrCount > 3) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();

        readBmsData(id);
        if (isAirUnitLeadCluster(id)) {
            readAirData(id);
            write_air_data(db, id);
        }
        write_bms_data(db, id);

        if (lastCellRealtimeTime[id] == std::chrono::steady_clock::time_point{} ||
            now - lastCellRealtimeTime[id] >=
                std::chrono::milliseconds(Config::BMS_CELL_REALTIME_INTERVAL)) {
            readBmsCellData(id);
            write_cell_data(db, id);
            lastCellRealtimeTime[id] = now;
        }

        if (lastBmsHistoryTime[id] == std::chrono::steady_clock::time_point{} ||
            now - lastBmsHistoryTime[id] >= std::chrono::seconds(30)) {
            write_Bmshistorydata(id);
            if (isAirUnitLeadCluster(id)) {
                const int airUnitId = bmsClusterToAirUnitId(id);
                const auto airNow = std::chrono::steady_clock::now();
                if (lastAirHistoryTime[airUnitId] == std::chrono::steady_clock::time_point{} ||
                    airNow - lastAirHistoryTime[airUnitId] >= std::chrono::seconds(10)) {
                    airHistorydata(airUnitId);
                    lastAirHistoryTime[airUnitId] = airNow;
                }
            }
            if (lastCellHistoryTime[id] == std::chrono::steady_clock::time_point{} ||
                now - lastCellHistoryTime[id] >= std::chrono::seconds(180)) {
                write_Cell_historydata(id);
                lastCellHistoryTime[id] = now;
            }
            lastBmsHistoryTime[id] = now;
        }

    } catch (const std::exception& e) {
        std::cerr << "Error in processAllBmsOperations (id=" << id << "): " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "Unknown error in processAllBmsOperations (id=" << id << ")" << std::endl;
    }
}

void Bamu::runBamuDataThread(MySQLConnectionPool& pool)
{
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto cyclePeriod = std::chrono::milliseconds(Config::BAMU_DATA_INTERVAL);
    while (true) {
        const auto cycleStart = std::chrono::steady_clock::now();
        try {
            MySQLDatabase db(pool);
            processAllBamuOperations(db);
            const int clusterCount = clampBmsClusterCount(batteryNumber);
            for (int i = 1; i <= clusterCount; ++i) {
                processAllBmsOperations(db, i);
            }
        } catch (const std::exception& e) {
            std::cerr << "BAMU数据线程错误: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "BAMU数据线程未知错误" << std::endl;
        }
        const auto elapsed = std::chrono::steady_clock::now() - cycleStart;
        if (elapsed < cyclePeriod) {
            std::this_thread::sleep_for(cyclePeriod - elapsed);
        }
    }
}

