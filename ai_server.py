import socket
import dashscope
import os
import json
import wave
import io
import struct
import requests
import time
import traceback
from pathlib import Path

# ================= 配置区 =================
# 建议将 API KEY 放入环境变量，不要直接写在代码里
API_KEY = "sk-f8cf143d90c248a9871c7da220dad9da"
dashscope.api_key = API_KEY

HOST = '0.0.0.0'
PORT = 8080

user_home = os.path.expanduser("~")
desktop_path = os.path.join(user_home, "Desktop")
if not os.path.exists(desktop_path):
    desktop_path = os.path.join(user_home, "桌面")
if not os.path.exists(desktop_path):
    desktop_path = user_home

RECEIVED_AUDIO_FILE = os.path.join(desktop_path, "received_audio.wav")

COLOR_GREEN = "\033[32m"
COLOR_BLUE = "\033[34m"
COLOR_YELLOW = "\033[33m"
COLOR_RED = "\033[31m"
COLOR_RESET = "\033[0m"

# ================= IR 编码逻辑 =================

# 温度映射 (摄氏度 -> 4位Hex值)
TEMP_MAP = {
    17: 0x0, 18: 0x1, 19: 0x3, 20: 0x2, 21: 0x6, 22: 0x7, 23: 0x5, 24: 0x4,
    25: 0xC, 26: 0xD, 27: 0x9, 28: 0x8, 29: 0xA, 30: 0xB
}

# 模式映射 (模式名 -> 4位Hex值)
# 注意：送风通常与除湿相同或温度位有特殊设定，此处暂按文档映射为4
MODE_MAP = {
    "制冷": 0x0,
    "除湿": 0x4,
    "送风": 0x4,
    "自动": 0x8,
    "制热": 0xC
}

# 风速映射 (风速名 -> 8位Hex值，通常高3位有效)
FAN_MAP = {
    "自动": 0xA0,
    "低": 0xE0,
    "中": 0x80,
    "高": 0x40
}

def generate_ir_code(command_data):
    """
    根据指令生成 QD-HS6324 协议的 6字节 Hex 码
    返回格式: 字符串 "B24DA05FD02F" 或 None
    """
    if not command_data or not command_data.get("has_command"):
        return None
    
    target = command_data.get("target")
    if target != "空调":
        return None

    action = command_data.get("action")
    value = command_data.get("value")

    # 默认状态
    current_temp = 26
    current_mode = "制冷" # 默认制冷比较常用
    current_fan = "自动"

    # 解析 value (尝试推断是温度、风速还是模式)
    # AI 返回的 value 可能是 "25", "高", "制冷" 等
    if value:
        # 尝试解析数字 (温度)
        if value.isdigit():
            val_int = int(value)
            # 是否在合理温度范围内
            if 17 <= val_int <= 30:
                current_temp = val_int
        # 尝试解析风速
        elif value in ["高", "中", "低", "自动"]:
            current_fan = value
        # 尝试解析模式 (虽然 System Prompt 没明确说 value 会出模式，但做个防御)
        elif value in ["制冷", "制热", "送风", "除湿"]:
            current_mode = value

    # 解析 action (如果是 "关闭" 等待处理，如果是 "制冷" 等模式词出现在 action)
    if action == "关闭":
        # 协议文档未提供明确的“关机”码，通常可能是特殊指令或模式
        # 这里仅打印提示，暂不发送红外以防错误
        print(f"{COLOR_RED}[IR] 警告: 协议文档未定义明确的'关机'码，跳过生成 IR。{COLOR_RESET}")
        return None
    
    # 构造 Byte 0 & 1 (Header)
    b0 = 0xB2
    b1 = 0x4D # ~B2

    # 构造 Byte 2 (Fan/Timer)
    # 既然 AI 没复杂的定时指令，默认定时为0，直接取风速码
    # 注意: 风速码低5位通常是定时器(0x1F或0)，这里参考文档 自动风=0xA0 (1010 0000), 低5位是0
    # 文档示例: 自动风 0xA0. 
    fan_hex = FAN_MAP.get(current_fan, 0xA0)
    
    # 若需定时逻辑可在此扩展，目前保持无定时
    b2 = fan_hex
    b3 = (~b2) & 0xFF # 取反

    # 构造 Byte 4 (Temp/Mode)
    # Temp 高4位, Mode 低4位
    temp_nibble = TEMP_MAP.get(current_temp, 0xD) # 默认 26 -> D
    mode_nibble = MODE_MAP.get(current_mode, 0x0) # 默认 制冷 -> 0
    
    b4 = (temp_nibble << 4) | mode_nibble
    b5 = (~b4) & 0xFF

    # 拼接结果
    raw_bytes = bytes([b0, b1, b2, b3, b4, b5])
    hex_str = raw_bytes.hex().upper()
    
    print(f"{COLOR_BLUE}[IR] 生成编码: {hex_str} (Temp:{current_temp}, Mode:{current_mode}, Fan:{current_fan}){COLOR_RESET}")
    return hex_str


def recv_all(sock, n):
    data = bytearray()
    while len(data) < n:
        try:
            packet = sock.recv(n - len(data))
            if not packet: return None
            data.extend(packet)
        except socket.timeout:
            print(f"{COLOR_RED}接收超时!{COLOR_RESET}")
            return None
        except Exception as e:
            print(f"{COLOR_RED}接收异常: {e}{COLOR_RESET}")
            return None
    return data


# [新] Hex 协议发送函数
# 对应 C++ 中的 hexCharToVal 解析逻辑 和 '*' 结束符
def send_as_hex_protocol(sock, data, label="Data"):
    if not data: return

    print(f"{COLOR_YELLOW}>>> [Protocol] 正在发送 {label} (Hex编码 + *结尾)...{COLOR_RESET}")

    # 1. 转换为 Hex 字符串的 bytes (例如 b'A1B2...')
    if isinstance(data, str):
        # 字符串先转 bytes 再转 hex
        hex_data = data.encode('utf-8').hex().upper().encode('utf-8')
    elif isinstance(data, bytes):
        # bytes 直接转 hex
        hex_data = data.hex().upper().encode('utf-8')
    else:
        print(f"{COLOR_RED}数据类型错误{COLOR_RESET}")
        return

    # 2. 分块发送 Hex 数据 (防止 ESP32 串口缓冲区溢出)
    chunk_size = 1024  # 每次发 1KB Hex 字符
    total = len(hex_data)
    sent = 0

    try:
        while sent < total:
            end = min(sent + chunk_size, total)
            sock.sendall(hex_data[sent:end])
            sent = end
            # 微小延时，给 ESP32 逐字节处理的时间 (C++里有 vTaskDelay)
            time.sleep(0.005)

            # 3. 发送协议结束符 '*'
        sock.sendall(b'*')
        print(f"{COLOR_GREEN}>>> {label} 发送完毕.{COLOR_RESET}")

    except Exception as e:
        print(f"{COLOR_RED}发送异常: {e}{COLOR_RESET}")
        raise e


def get_ai_analysis(file_path):
    print(f"{COLOR_BLUE}>>> [AI] 正在分析语音意图...{COLOR_RESET}")
    abs_path = Path(file_path).resolve()
    file_url = str(abs_path)
    if not os.path.exists(file_url): return None

    # 更新 Prompt，增加让 AI 识别属性的提示
    system_prompt = """
    你是一个车载智能助手。请分析用户的语音意图，控制"空调"。
    必须严格输出且仅输出一个合法的 JSON 对象。
    JSON 格式定义如下：
    {
        "reply": "这里是给用户的口语化回复，简短幽默，60字以内",
        "command": {
            "has_command": true/false, 
            "target": "空调" | "车窗" | "音乐" | "导航" | "无", 
            "action": "打开" | "关闭" | "调节" | "播放" | null, 
            "value": "25" | "高" | "低" | "制冷" | "制热" | null 
        }
    }
    注意：如果用户说“制冷模式”或“26度”，请尽量提取到 value 中。
    """
    messages = [
        {"role": "system", "content": [{"text": system_prompt}]},
        {"role": "user", "content": [{"audio": file_url}]}
    ]
    try:
        response = dashscope.MultiModalConversation.call(
            model="qwen-audio-turbo-latest",
            api_key=API_KEY,
            messages=messages,
        )
        if response.status_code == 200:
            content = response["output"]["choices"][0]["message"]["content"]
            raw_text = ""
            for item in content:
                if "text" in item: raw_text = item["text"]
            raw_text = raw_text.replace("```json", "").replace("```", "").strip()
            try:
                return json.loads(raw_text)
            except json.JSONDecodeError:
                return {"reply": "我没听懂，请再说一次。", "command": {"has_command": False}}
        else:
            print(f"{COLOR_RED}AI Fail: {response.code}{COLOR_RESET}")
            return None
    except Exception as e:
        print(f"{COLOR_RED}AI Exception: {e}{COLOR_RESET}")
        return None


def get_tts_pcm(text):
    if not text: return None
    print(f"{COLOR_YELLOW}正在生成语音: {text}{COLOR_RESET}")
    try:
        response = dashscope.MultiModalConversation.call(
            model="qwen3-tts-flash",
            api_key=API_KEY,
            text=text,
            voice="Cherry",
            format="wav",
            sample_rate=24000,
            stream=False
        )
        if response.status_code == 200 and response.output.audio:
            r = requests.get(response.output.audio['url'])
            # 读取 WAV 去掉头信息，只保留 PCM 数据
            with wave.open(io.BytesIO(r.content), 'rb') as wav_file:
                return wav_file.readframes(wav_file.getnframes())
        return None
    except Exception as e:
        print(f"{COLOR_RED}TTS Exception: {e}{COLOR_RESET}")
        return None


def start_server():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    try:
        server.bind((HOST, PORT))
        server.listen(1)
    except Exception as e:
        print(f"{COLOR_RED}端口绑定失败: {e}{COLOR_RESET}")
        return

    print(f"\n========================================")
    print(f"    智能车载中枢 (Hex协议 + 红外控制版)")
    print(f"    本地监听: {HOST}:{PORT}")
    print(f"========================================\n")

    while True:
        conn = None
        try:
            print(f"{COLOR_GREEN}>>> 等待 ESP32 (4G模式) 连接...{COLOR_RESET}")
            conn, addr = server.accept()
            print(f"连接来自: {addr}")
            conn.settimeout(60.0)

            # ---------------------------------------------------------
            # 1. 接收流程
            # ---------------------------------------------------------
            len_data = recv_all(conn, 4)
            if not len_data:
                conn.close()
                continue
            audio_len = struct.unpack('>I', len_data)[0]
            print(f"接收音频长度: {audio_len} bytes")

            if audio_len > 0:
                audio_data = recv_all(conn, audio_len)
                if not audio_data:
                    conn.close()
                    continue
                with open(RECEIVED_AUDIO_FILE, 'wb') as f:
                    f.write(audio_data)

            # ---------------------------------------------------------
            # 2. AI 处理流程
            # ---------------------------------------------------------
            conn.settimeout(None)
            ai_result = get_ai_analysis(RECEIVED_AUDIO_FILE)

            reply_text = "我没听清。"
            command_data = {"has_command": False}
            ir_code = None

            if ai_result:
                reply_text = ai_result.get("reply", "我在。")
                command_data = ai_result.get("command", command_data)
                
                # ==== 生成 IR 码 ====
                if command_data.get("has_command"):
                    ir_code = generate_ir_code(command_data)

            # 准备数据
            # 将 ir_code 加入到控制字段中
            final_control = {
                "has_command": command_data.get("has_command", False),
                "target": command_data.get("target"),
                "action": command_data.get("action"),
                "value": command_data.get("value"),
                "ir_code": ir_code # 新增字段: Hex String
            }

            resp_json = {"status": "ok", "reply_text": reply_text, "control": final_control}
            json_str = json.dumps(resp_json, ensure_ascii=False)
            pcm_bytes = get_tts_pcm(reply_text) or b''

            print(f"准备回复: JSON={len(json_str)} chars, Audio={len(pcm_bytes)} bytes")
            if ir_code:
                print(f"包含红外指令: {ir_code}")

            # ---------------------------------------------------------
            # 3. 发送流程
            # ---------------------------------------------------------

            print(f"{COLOR_YELLOW}>>> 状态切换等待 1秒...{COLOR_RESET}")
            time.sleep(1.0)

            # (A) 发送 JSON (Hex + *)
            send_as_hex_protocol(conn, json_str, "JSON控制指令")

            # (B) 发送 音频 (Hex + *)
            if len(pcm_bytes) > 0:
                send_as_hex_protocol(conn, pcm_bytes, "TTS语音流")

            # ---------------------------------------------------------
            # 4. 结束连接
            # ---------------------------------------------------------
            print(f"{COLOR_BLUE}>>> 交互完成，关闭连接{COLOR_RESET}\n")
            time.sleep(1.0)
            conn.close()

        except Exception as e:
            print(f"{COLOR_RED}异常: {e}{COLOR_RESET}")
            traceback.print_exc()
        if conn: conn.close()


if __name__ == '__main__':
    start_server()
