#include "App_IR.h"

AppIR MyIR;

const uint16_t kCaptureBufferSize = 1024; 
const uint8_t kTimeout = 20; 

extern QueueHandle_t IRQueue_Handle; 

// 1. 全局只保留 ac 对象，它会独占管理 PIN_IR_TX
IRElectraAc ac(PIN_IR_TX); 

void AppIR::init() {
    Serial.println("[IR] Initializing...");

    // --- 接收部分 (不变) ---
    _irRecv = new IRrecv(PIN_IR_RX, kCaptureBufferSize, kTimeout, true);
    _irRecv->enableIRIn(); 

    // --- 发送部分 (关键修改) ---
    // [删除] 不要在这里 new IRsend，会和 ac 对象冲突！
    // _irSend = new IRsend(PIN_IR_TX);
    // _irSend->begin(); 

    // 初始化空调对象 (它会自动初始化 RMT 驱动)
    Serial.println("IR: Initializing Electra AC...");
    ac.begin(); 
    
    // 初始化默认状态
    ac.off();
    ac.setFan(kElectraAcFanAuto);
    ac.setMode(kElectraAcCool);
    ac.setTemp(26);
    ac.setSwingV(kElectraAcSwingOff);
    ac.setSwingH(kElectraAcSwingOff);

    Serial.printf("[IR] Driver Started. RX:%d, TX:%d\n", PIN_IR_RX, PIN_IR_TX);
}

// 专门控制空调的函数
void App_IR_Control_AC(bool power, uint8_t temp)
{
    Serial.printf("IR: Setting AC Power=%d, Temp=%d\n", power, temp);

    // 暂停接收，防止干扰发送
    // _irRecv->disableIRIn(); 

    // 1. 设置电源
    if (power) {
        ac.on();
    } else {
        ac.off();
    }

    // 2. 设置模式
    ac.setMode(kElectraAcCool);

    // 3. 设置风速
    ac.setFan(kElectraAcFanAuto);

    // 4. 设置温度
    ac.setTemp(temp);

    // 5. 发送信号
    ac.send();
    
    // 恢复接收
    // _irRecv->enableIRIn();

    Serial.println("IR: Signal Sent (Electra Protocol)!");
    Serial.println(ac.toString().c_str()); 
}

// 测试函数
void App_IR_Test_Send(void)
{
    App_IR_Control_AC(true, 20);
}

void AppIR::loop() {
    if (_irRecv->decode(&_results)) {
        if (_results.value != kRepeat) {
            IREvent evt;
            memset(&evt, 0, sizeof(IREvent));
            evt.protocol = _results.decode_type;
            evt.bits = _results.bits;
            evt.value = _results.value;
            evt.isAC = false;

            if (_results.decode_type == COOLIX || _results.bits > 64) {
                evt.isAC = true;
                int byteCount = _results.bits / 8;
                if (_results.bits % 8 != 0) byteCount++; 
                if (byteCount > IR_STATE_SIZE) byteCount = IR_STATE_SIZE; 
                for (int i = 0; i < byteCount; i++) evt.state[i] = _results.state[i];
                Serial.printf("[IR] AC Signal Detected (Protocol: %s)\n", typeToString(_results.decode_type).c_str());
            } else {
                Serial.printf("[IR] Normal Signal: 0x%llX\n", _results.value);
            }
            if (IRQueue_Handle != NULL) xQueueSend(IRQueue_Handle, &evt, 0);
        } 
        _irRecv->resume(); 
    }
}

// 修改后的 sendNEC：为了兼容性，临时创建一个 sender
void AppIR::sendNEC(uint32_t data) {
    Serial.printf("[IR] Sending NEC: 0x%08X\n", data);
    
    // 临时创建一个 sender 来发送 NEC，避免全局冲突
    // 注意：频繁创建销毁开销较大，但对于偶尔控制灯是没问题的
    IRsend tempSender(PIN_IR_TX);
    tempSender.begin();
    tempSender.sendNEC(data, 32);
    
    // 发送完 NEC 后，必须重新初始化 AC 对象，否则 AC 下次可能发不出信号
    // 因为 tempSender 销毁时可能会重置引脚状态
    ac.begin(); 

    vTaskDelay(pdMS_TO_TICKS(20)); 
    _irRecv->enableIRIn(); 
}

void AppIR::sendCoolix(uint32_t data) {
    // 同样的逻辑
    IRsend tempSender(PIN_IR_TX);
    tempSender.begin();
    tempSender.sendCOOLIX(data);
    ac.begin(); // 恢复 AC 驱动
    
    vTaskDelay(pdMS_TO_TICKS(50)); 
    _irRecv->enableIRIn(); 
    Serial.println("[IR] Coolix Sent.");
}

// [新增] 实现 QD-HS6324 协议发送
// 参数: hexStr 例如 "B24DA05FD02F"
void AppIR::sendQDHSString(String hexStr) {
    if (hexStr.length() == 0 || hexStr.length() % 2 != 0) {
        Serial.println("[IR] Error: Invalid Hex String");
        return;
    }

    Serial.printf("[IR] Sending QD-HS6324 Raw: %s\n", hexStr.c_str());

    // 1. 解析 Hex 串 -> Bytes
    int len = hexStr.length() / 2;
    uint8_t* dataBytes = (uint8_t*)malloc(len);
    for (int i = 0; i < len; i++) {
        char high = hexStr[i * 2];
        char low = hexStr[i * 2 + 1];
        
        uint8_t hVal = (high >= 'A') ? (high - 'A' + 10) : (high - '0');
        if (high >= 'a') hVal = high - 'a' + 10; // 支持小写
        
        uint8_t lVal = (low >= 'A') ? (low - 'A' + 10) : (low - '0');
        if (low >= 'a') lVal = low - 'a' + 10;
        
        dataBytes[i] = (hVal << 4) | lVal;
    }

    // 2. 构造 Raw Buffer
    // 协议时序 (us):
    // Leader: 4350, 4350
    // 0: 580, 580
    // 1: 580, 1580
    // Stop: 580, 5290 (仅在多帧中间用，这里暂定单帧)
    // End: 580 (Header Mark for shutdown? No, it's trail mark)
    // 实际: 单帧 = Leader + Data + EndMark (580us)
    
    // 计算 buffer size: Header(2) + Bits(len*8)*2 + Footer(1)
    // uint16_t bufferSize = 2 + len * 8 * 2 + 1; 
    // 使用 vector 或 dynamic array
    uint16_t* rawBuf = new uint16_t[200]; // 够用就行
    int idx = 0;

    // A. Leader
    rawBuf[idx++] = 4350;
    rawBuf[idx++] = 4350;

    // B. Data
    for (int i = 0; i < len; i++) {
        for (int b = 7; b >= 0; b--) { // MSB First
            rawBuf[idx++] = 580; // Mark
            if ((dataBytes[i] >> b) & 1) {
                rawBuf[idx++] = 1580; // Space for 1
            } else {
                rawBuf[idx++] = 580;  // Space for 0
            }
        }
    }

    // C. End Mark
    rawBuf[idx++] = 580; 

    // 3. 发送
    IRsend tempSender(PIN_IR_TX);
    tempSender.begin();
    tempSender.sendRaw(rawBuf, idx, 38); // 38kHz

    // 4. 清理
    delete[] rawBuf;
    free(dataBytes);

    // 5. 恢复 AC
    ac.begin();
    vTaskDelay(pdMS_TO_TICKS(50));
    _irRecv->enableIRIn();
    
    Serial.println("[IR] QD-HS6324 Sent.");
}