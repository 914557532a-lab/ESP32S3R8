#ifndef APP_IR_H
#define APP_IR_H

#include <Arduino.h>
#include "Pin_Config.h" 
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <ir_Electra.h>
#include <IRutils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// 定义最大空调数据长度 (字节)
// AUX通常是13字节(104位)，我们给大一点32字节(256位)以防万一
#define IR_STATE_SIZE 32


void App_IR_Test_Send(void); // 用于测试发送

// 新增：专门控制空调的函数
void App_IR_Control_AC(bool power, uint8_t temp);

// 定义一个红外事件结构体
struct IREvent {
    decode_type_t protocol;     // 协议类型 (NEC, AUX, etc.)
    uint64_t value;             // 电视/普通遥控器用的短码 (比如 NEC)
    uint8_t state[IR_STATE_SIZE]; // 空调专用：存储长编码状态
    uint16_t bits;              // 位数
    bool isAC;                  // 标记是否为空调/长码信号
};

class AppIR {
public:
    void init();
    void loop();
    
    // 发送普通 NEC 信号
    void sendNEC(uint32_t data);
    
    // 【新增】发送 AUX 空调信号
    // data: 字节数组, len: 字节长度(通常AUX是13)
    void sendCoolix(uint32_t data);

    // [新增] 发送 QD-HS6324 (Hex String format: "B24DA0...")
    void sendQDHSString(String hexStr);

private:
    IRrecv* _irRecv = nullptr;
    IRsend* _irSend = nullptr;
    decode_results _results;
};

extern AppIR MyIR;

#endif