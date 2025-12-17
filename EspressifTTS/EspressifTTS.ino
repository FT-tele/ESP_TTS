#include <Arduino.h>
#include "driver/i2s.h"
#include "esp_tts.h"
#include "esp_partition.h"

// --- 链接符号声明 ---
extern "C" {
    extern const esp_tts_voice_t esp_tts_voice_xiaole;
}

// I2S 引脚配置 (ESP32-S3)
#define I2S_NUM         I2S_NUM_0
#define BCLK_PIN        48
#define WS_PIN          45
#define DOUT_PIN        47

static esp_tts_handle_t tts_handle = NULL;

void init_i2s() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 8000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 6,
        .dma_buf_len = 1024, 
        .use_apll = false
    };
    i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL);
    i2s_pin_config_t pin_config = {
        .bck_io_num = BCLK_PIN,
        .ws_io_num = WS_PIN,
        .data_out_num = DOUT_PIN,
        .data_in_num = -1
    };
    i2s_set_pin(I2S_NUM, &pin_config);
}

void speak(const char* text) {
    if (tts_handle == NULL) return;

    Serial.printf(">>> 正在合成: [%s]\n", text);
    
    if (esp_tts_parse_chinese(tts_handle, (char*)text)) {
        int len[1];
        // 1. 正常播放语音流
        do {
            short *pcm = esp_tts_stream_play(tts_handle, len, 4);
            if (len[0] > 0) {
                size_t bytes_written;
                i2s_write(I2S_NUM, pcm, len[0] * 2, &bytes_written, portMAX_DELAY);
            }
        } while (len[0] > 0);

        /**
         * 2. 核心修复：静音冲刷 (Flush)
         * 我们手动构造一段静音数据（全为 0），长度建议为 DMA 缓冲区总大小的 1.5 倍。
         * 这样可以确保最后的语音被完整挤出 I2S 硬件，而 DMA 循环时播放的是静音。
         */
        size_t bytes_written;
        // 构造约 200ms 的静音数据 (16位采样, 12000Hz)
        short silence_buffer[1200] = {0}; 
        
        // 连续发送两次静音，彻底填满 DMA 管道
        i2s_write(I2S_NUM, silence_buffer, sizeof(silence_buffer), &bytes_written, portMAX_DELAY);
        i2s_write(I2S_NUM, silence_buffer, sizeof(silence_buffer), &bytes_written, portMAX_DELAY);

        // 3. 此时无需再用长 delay，只需极短的等待让硬件处理完最后的静音
        delay(500); 
        
        // 4. 最后清理，万无一失
        i2s_zero_dma_buffer(I2S_NUM);
        
        Serial.println("--- 播报完成 (已自动清理) ---");
    }
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("\n--- ESP32-S3 TTS 串口实时版 ---");

    init_i2s();

    // 1. 挂载分区
    const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "voice_data");
    if (!part) {
        Serial.println("❌ 找不到分区");
        return;
    }

    esp_partition_mmap_handle_t mmap_handle;
    const void* voicedata_ptr = NULL;
    if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &voicedata_ptr, &mmap_handle) != ESP_OK) {
        Serial.println("❌ Mmap 失败");
        return;
    }

    // 2. 初始化引擎
    esp_tts_voice_t *voice = esp_tts_voice_set_init(&esp_tts_voice_xiaole, (void*)voicedata_ptr);
    tts_handle = esp_tts_create(voice);

    if (tts_handle) {
        // 注意：由于当前环境库版本限制，已移除 set_speed 和 set_volume
        // 引擎将以默认参数运行
        Serial.println("✅ TTS 引擎就绪！");
        Serial.println("请在串口监视器（需开启 NL & CR）输入中文文本...");
        speak("系统已启动，请开始输入。");
    } else {
        Serial.println("❌ 引擎创建失败");
    }
}

void loop() {
    // 监听串口输入并实时播报
    if (Serial.available() > 0) {
        String input = Serial.readStringUntil('\n');
        input.trim();
        if (input.length() > 0) {
            speak(input.c_str());
        }
    }
}