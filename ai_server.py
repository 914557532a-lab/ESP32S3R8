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
from dashscope.audio.tts import SpeechSynthesizer 

# ================= 配置区 =================
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
TEMP_MAP = {17:0x0,18:0x1,19:0x3,20:0x2,21:0x6,22:0x7,23:0x5,24:0x4,25:0xC,26:0xD,27:0x9,28:0x8,29:0xA,30:0xB}
MODE_MAP = {"制冷":0x0,"除湿":0x4,"送风":0x4,"自动":0x8,"制热":0xC}
FAN_MAP = {"自动":0xA0,"低":0xE0,"中":0x80,"高":0x40}

def generate_ir_code(command_data):
    if not command_data or not command_data.get("has_command"): return None
    if command_data.get("target") != "空调": return None
    action, value = command_data.get("action"), command_data.get("value")
    current_temp, current_mode, current_fan = 26, "制冷", "自动"
    if value:
        if value.isdigit() and 17 <= int(value) <= 30: current_temp = int(value)
        elif value in ["高", "中", "低", "自动"]: current_fan = value
        elif value in ["制冷", "制热", "送风", "除湿"]: current_mode = value
    if action == "关闭": return None
    
    b0, b1 = 0xB2, 0x4D
    b2 = FAN_MAP.get(current_fan, 0xA0)
    b3 = (~b2) & 0xFF
    b4 = (TEMP_MAP.get(current_temp, 0xD) << 4) | MODE_MAP.get(current_mode, 0x0)
    b5 = (~b4) & 0xFF
    return bytes([b0, b1, b2, b3, b4, b5]).hex().upper()

# ================= IMA ADPCM 编码器 =================
index_table = [
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
]

step_size_table = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3326, 3658, 4024, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
]

class AdpcmState:
    def __init__(self):
        self.valprev = 0
        self.index = 0

def encode_adpcm_block(pcm_data, state):
    result = bytearray()
    if len(pcm_data) % 2 != 0: pcm_data = pcm_data[:-1]
    samples = struct.unpack(f"<{len(pcm_data)//2}h", pcm_data)
    
    for i in range(0, len(samples), 2):
        byte_val = 0
        
        # Low Nibble
        if i < len(samples):
            val = samples[i]
            diff = val - state.valprev
            step = step_size_table[state.index]
            delta = 0
            if diff < 0:
                delta = 8
                diff = -diff
            vpdiff = (step >> 3)
            if diff >= step:
                delta |= 4
                diff -= step
                vpdiff += step
            if diff >= (step >> 1):
                delta |= 2
                diff -= (step >> 1)
                vpdiff += (step >> 1)
            if diff >= (step >> 2):
                delta |= 1
                vpdiff += (step >> 2)
            if delta & 8: state.valprev -= vpdiff
            else: state.valprev += vpdiff
            if state.valprev > 32767: state.valprev = 32767
            elif state.valprev < -32768: state.valprev = -32768
            state.index += index_table[delta]
            if state.index < 0: state.index = 0
            if state.index > 88: state.index = 88
            byte_val = delta
            
        # High Nibble
        if i+1 < len(samples):
            val = samples[i+1]
            diff = val - state.valprev
            step = step_size_table[state.index]
            delta = 0
            if diff < 0:
                delta = 8
                diff = -diff
            vpdiff = (step >> 3)
            if diff >= step:
                delta |= 4
                diff -= step
                vpdiff += step
            if diff >= (step >> 1):
                delta |= 2
                diff -= (step >> 1)
                vpdiff += (step >> 1)
            if diff >= (step >> 2):
                delta |= 1
                vpdiff += (step >> 2)
            if delta & 8: state.valprev -= vpdiff
            else: state.valprev += vpdiff
            if state.valprev > 32767: state.valprev = 32767
            elif state.valprev < -32768: state.valprev = -32768
            state.index += index_table[delta]
            if state.index < 0: state.index = 0
            if state.index > 88: state.index = 88
            byte_val |= (delta << 4)

        result.append(byte_val)
    return result

# ================= IMA ADPCM 解码器 (上传用) =================
def decode_adpcm_block(adpcm_data, state):
    # adpcm_data: bytes object
    # Returns: bytes object (PCM int16)
    result = bytearray()
    
    for byte_val in adpcm_data:
        # Sample 1 (Low Nibble)
        delta = byte_val & 0x0F
        step = step_size_table[state.index]
        diffq = step >> 3
        if delta & 4: diffq += step
        if delta & 2: diffq += (step >> 1)
        if delta & 1: diffq += (step >> 2)
        
        if delta & 8: state.valprev -= diffq
        else: state.valprev += diffq
        
        if state.valprev > 32767: state.valprev = 32767
        elif state.valprev < -32768: state.valprev = -32768
        
        state.index += index_table[delta]
        if state.index < 0: state.index = 0
        if state.index > 88: state.index = 88
        
        result.extend(struct.pack('<h', int(state.valprev)))

        # Sample 2 (High Nibble)
        delta = (byte_val >> 4) & 0x0F
        step = step_size_table[state.index]
        diffq = step >> 3
        if delta & 4: diffq += step
        if delta & 2: diffq += (step >> 1)
        if delta & 1: diffq += (step >> 2)
        
        if delta & 8: state.valprev -= diffq
        else: state.valprev += diffq
        
        if state.valprev > 32767: state.valprev = 32767
        elif state.valprev < -32768: state.valprev = -32768
        
        state.index += index_table[delta]
        if state.index < 0: state.index = 0
        if state.index > 88: state.index = 88
        
        result.extend(struct.pack('<h', int(state.valprev)))
        
    return result

# ================= 业务函数 =================

def recv_all(sock, n):
    data = bytearray()
    while len(data) < n:
        try:
            packet = sock.recv(n - len(data))
            if not packet: return None
            data.extend(packet)
        except: return None
    return data

def get_ai_analysis(file_path):
    print(f"{COLOR_BLUE}>>> [AI] 分析语音...{COLOR_RESET}")
    file_url = str(Path(file_path).resolve())
    system_prompt = """
    你是一个车载智能助手。请分析用户的语音意图，控制"空调"。
    必须严格输出且仅输出一个合法的 JSON 对象。
    JSON 格式定义如下：
    {
        "reply": "这里是给用户的口语化回复，简短幽默，30字以内",
        "command": {
            "has_command": true/false, 
            "target": "空调", 
            "action": "打开" | "关闭" | "调节" | null, 
            "value": "25" | "高" | "低" | "制冷" | "制热" | null 
        }
    }
    """
    try:
        response = dashscope.MultiModalConversation.call(
            model="qwen-audio-turbo-latest",
            api_key=API_KEY,
            messages=[
                {"role": "system", "content": [{"text": system_prompt}]},
                {"role": "user", "content": [{"audio": file_url}]}
            ]
        )
        if response.status_code == 200:
            content = response["output"]["choices"][0]["message"]["content"]
            txt = content[0]["text"] if isinstance(content, list) else str(content)
            txt = txt.replace("```json", "").replace("```", "").strip()
            return json.loads(txt)
    except Exception as e:
        print(f"AI Error: {e}")
    return {"reply": "我没听清。", "command": {"has_command": False}}

from dashscope.audio.tts import SpeechSynthesizer 

# ... (Previous Code) ...

def get_tts_stream_chunk(text):
    """ 生成器：生成 16000Hz PCM 原始数据 """
    print(f"{COLOR_YELLOW}>>> [TTS] 生成: {text} (16kHz Stream){COLOR_RESET}")
    try:
        result = SpeechSynthesizer.call(
            model='sambert-zhichu-v1',
            text=text,
            sample_rate=16000,
            format='pcm'
        )
        if result.get_audio_data() is not None:
             yield result.get_audio_data()
    except Exception as e:
        print(f"TTS Error: {e}")

# ================= 服务端主逻辑 =================

def send_hex_protocol(sock, text_data):
    """ Phase 1: 发送 JSON (Old School Hex) 用于兼容 """
    try:
        hex_str = text_data.encode('utf-8').hex().upper().encode('utf-8')
        sock.sendall(hex_str)
        time.sleep(0.01)
        sock.sendall(b'*') # End of JSON
    except Exception as e:
        print(f"Protocol Error: {e}")

def start_server():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        server.bind((HOST, PORT))
        server.listen(1)
    except Exception as e:
        print(f"Port Error: {e}")
        return
    
    print(f"\n======== [ESP32 极速流式服务端 (双向ADPCM)] ========")
    print(f"监听: {HOST}:{PORT}")
    print(f"下行: 16kHz ADPCM Stream")
    print(f"上行: ADPCM Upload -> Decode -> WAV")
    print(f"================================================\n")

    while True:
        conn = None
        try:
            print(f"{COLOR_GREEN}等待连接...{COLOR_RESET}")
            conn, addr = server.accept()
            conn.settimeout(60.0)
            
            # 1. 接收录音 (ADPCM Compressed)
            len_data = recv_all(conn, 4)
            if not len_data: 
                conn.close()
                continue
            
            compressed_len = struct.unpack('>I', len_data)[0]
            print(f"接收 ADPCM: {compressed_len} bytes")
            
            if compressed_len > 0:
                adpcm_data = recv_all(conn, compressed_len)
                
                # [Upload Optimize] 解码 ADPCM -> PCM
                decode_state = AdpcmState()
                pcm_data = decode_adpcm_block(adpcm_data, decode_state)
                
                # 保存为标准 WAV (16kHz, Mono, 16bit)
                # ESP32 采集也是单声道的 (虽然 i2s config channels=2 in setup but read 2 bytes, Adpcm uses 2 samples per byte)
                # Correction: App_Audio uses channels=2 in i2s config but reads simply. Let's assume Mono for upload or match logic.
                # In App_Audio: i2s config channels=2.
                # recordTask reads 1024 bytes -> 512 samples. 
                # If channels=2, that's 256 frames (L+R). 
                # Adpcm encode simply takes s1, s2. So it encodes whatever comes in.
                # If I2S is stereo, we are encoding I2S raw stream. 
                # Decoding will produce the same I2S raw stream. 
                # Use channels=2 for WAV header to match source.
                
                with wave.open(RECEIVED_AUDIO_FILE, 'wb') as wav_file:
                    wav_file.setnchannels(2) 
                    wav_file.setsampwidth(2)
                    wav_file.setframerate(16000)
                    wav_file.writeframes(pcm_data)
                
                print(f"解码并保存: {len(pcm_data)} bytes PCM")
            ai_res = get_ai_analysis(RECEIVED_AUDIO_FILE)
            reply = ai_res.get("reply", "我在")
            cmd = ai_res.get("command", {}) 
            ir_code = generate_ir_code(cmd)
            
            resp_json = {
                "status": "ok", 
                "reply_text": reply, 
                "control": {
                    "has_command": cmd.get("has_command", False),
                    "target": cmd.get("target"),
                    "action": cmd.get("action"),
                    "value": cmd.get("value"),
                    "ir_code": ir_code 
                }
            }
            json_str = json.dumps(resp_json, ensure_ascii=False)
            
            # 3. 发送 JSON
            print(f"回复 JSON: {json_str}")
            send_hex_protocol(conn, json_str)
            
            # 4. 发送 音频流
            print(f"{COLOR_BLUE}>>> 开始推送 ADPCM 音频流...{COLOR_RESET}")
            adpcm = AdpcmState()
            total_sent = 0
            
            for pcm_chunk in get_tts_stream_chunk(reply):
                if not pcm_chunk: continue
                # 编码: PCM -> ADPCM
                encoded_chunk = encode_adpcm_block(pcm_chunk, adpcm)
                
                if len(encoded_chunk) > 0:
                    try:
                         # 分包推送
                         CHUNK = 1024
                         for i in range(0, len(encoded_chunk), CHUNK):
                             part = encoded_chunk[i:i+CHUNK]
                             conn.sendall(part)
                             time.sleep(0.01) # 流控
                         total_sent += len(encoded_chunk)
                    except OSError:
                        print("连接断开")
                        break
                        
            print(f"{COLOR_GREEN}>>> 传输完成: {total_sent} bytes (ADPCM){COLOR_RESET}")
            
            # 5. 关闭
            time.sleep(0.5)
            conn.close()
            print("连接关闭.\n")

        except Exception as e:
            print(f"Err: {e}")
            traceback.print_exc()
            if conn: conn.close()

if __name__ == '__main__':
    start_server()
