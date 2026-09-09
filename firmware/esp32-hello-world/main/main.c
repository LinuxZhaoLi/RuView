/**
 * @file main.c
 * @brief ESP32 你好，世界-全能力发现
 *
 * 打印"你好，世界！"，然后探查芯片信息、Flash、PSRAM、WiFi（包括CSI）、802.15.4/BLE on C6, GPIOs,
 * peripherals, FreeRTOS stats, and power management.  No WiFi connection
 * required.  Supports ESP32-S3 and ESP32-C6 (set IDF target accordingly).
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_efuse.h"
#include "esp_pm.h"
#include "nvs_flash.h"
#include "soc/soc_caps.h"
#include "driver/gpio.h"
#include "driver/temperature_sensor.h"
#include "sdkconfig.h"

/*
 * 外设数量: ESP-IDF v6+ dropped some SOC_* macros; values below
 * 匹配每个目标的HAL（esp_hal_* *_ll.h），如果适用。
 */
#if CONFIG_IDF_TARGET_ESP32S3
#define PROBE_I2S_CTRL_NUM   2
#define PROBE_RMT_CHAN_NUM   8
#define PROBE_MCPWM_GROUPS   2
#define PROBE_PCNT_UNITS     4
#define PROBE_TOUCH_CHAN_NUM ((int)(SOC_TOUCH_MAX_CHAN_ID - SOC_TOUCH_MIN_CHAN_ID + 1))
#elif CONFIG_IDF_TARGET_ESP32C6
#define PROBE_I2S_CTRL_NUM   1
#define PROBE_RMT_CHAN_NUM   4
#define PROBE_MCPWM_GROUPS   1
#define PROBE_PCNT_UNITS     4
#else
#error "hello-world: add 添加 PROBE_* 定义，用于当前 IDF 目标的外设数量，然后在 main.c 中包含此文件"
#endif

/* ── Helpers ─────────────────────────────────────────────────────────── */

static const char *chip_model_str(esp_chip_model_t model)
{
    switch (model) {
        case CHIP_ESP32:   return "ESP32";
        case CHIP_ESP32S2: return "ESP32-S2";
        case CHIP_ESP32S3: return "ESP32-S3";
        case CHIP_ESP32C3: return "ESP32-C3";
        case CHIP_ESP32H2: return "ESP32-H2";
        case CHIP_ESP32C2: return "ESP32-C2";
        case CHIP_ESP32C6: return "ESP32-C6";
        default:           return "Unknown";
    }
}
// 打印分隔线
static void print_separator(const char *title)
{
    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║  %-55s ║\n", title);
    printf("╚══════════════════════════════════════════════════════════╝\n");
}

/* ── 能力探查 ─────────────────────────────────────────────────── */

static void probe_chip_info(void)
{
    print_separator("芯片信息探查");

    esp_chip_info_t info;
    esp_chip_info(&info);

    printf("  Model:          %s (rev %d.%d)\n",
           chip_model_str(info.model),
           info.revision / 100, info.revision % 100);
    printf("  Cores:          %d\n", info.cores);
    printf("  Features:       ");
    if (info.features & CHIP_FEATURE_WIFI_BGN) printf("WiFi ");
    if (info.features & CHIP_FEATURE_BLE)      printf("BLE ");
    if (info.features & CHIP_FEATURE_BT)       printf("BT-Classic ");
    if (info.features & CHIP_FEATURE_IEEE802154) printf("802.15.4 ");
    if (info.features & CHIP_FEATURE_EMB_FLASH) printf("EmbFlash ");
    if (info.features & CHIP_FEATURE_EMB_PSRAM) printf("EmbPSRAM ");
    printf("\n");

    /* MAC addresses */
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        printf("  WiFi STA MAC:   %02X:%02X:%02X:%02X:%02X:%02X\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) {
        printf("  BT MAC:         %02X:%02X:%02X:%02X:%02X:%02X\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    printf("  IDF Version:    %s\n", esp_get_idf_version());
    printf("  Reset Reason:   %d\n", esp_reset_reason());
}
// 内存信息探查
static void probe_memory(void)
{
    print_separator("内存信息探查");

    /* Internal RAM */
    printf("  Internal DRAM:\n");
    printf("    Total:        %"PRIu32" bytes\n",
           (uint32_t)heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    printf("    Free:         %"PRIu32" bytes\n",
           (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    printf("    Min Free:     %"PRIu32" bytes\n",
           (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));

    /* PSRAM */
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_total > 0) {
        printf("  External PSRAM:\n");
        printf("    Total:        %"PRIu32" bytes (%.1f MB)\n",
               (uint32_t)psram_total, psram_total / (1024.0 * 1024.0));
        printf("    Free:         %"PRIu32" bytes\n",
               (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    } else {
        printf("  External PSRAM: Not available\n");
    }

    /* DMA-capable */
    printf("  DMA-capable:    %"PRIu32" bytes free\n",
           (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DMA));
}
// FLASH 存储信息探查
static void probe_flash(void)
{
    print_separator("FLASH存储信息探查");

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        printf("  Flash Size:     %"PRIu32" bytes (%.0f MB)\n",
               flash_size, flash_size / (1024.0 * 1024.0));
    }

    /* Partition table */
    printf("  Partition Table:\n");
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY,
                                                     ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it != NULL) {
        const esp_partition_t *p = esp_partition_get(it);
        printf("    %-16s type=0x%02x sub=0x%02x offset=0x%06"PRIx32" size=%"PRIu32" KB\n",
               p->label, p->type, p->subtype, p->address, p->size / 1024);
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);

    /* 运行中的分区 */
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        printf("  Running from:   %s (0x%06"PRIx32")\n", running->label, running->address);
    }
}
// WiFi 能力探查
static void probe_wifi_capabilities(void)
{
    print_separator("WiFi能力探查");

    /* Init WiFi just enough to query capabilities (no connection) */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Protocol capabilities */
#if CONFIG_IDF_TARGET_ESP32C6
    printf("  Protocols:      802.11 b/g/n/ax (Wi-Fi 6, 2.4 GHz)\n");
#else
    printf("  Protocols:      802.11 b/g/n\n");
#endif

    /* CSI (Channel State Information) */
#ifdef CONFIG_ESP_WIFI_CSI_ENABLED
    printf("  CSI:            ENABLED (信道状态信息)\n");
    printf("    - S副载波振幅和相位数据\n");
    printf("    - 每包回调可用\n");
    printf("    - 用于： 人员检测、手势识别、呼吸率、室内定位\n");
#else
    printf("  CSI:            DISABLED (enable CONFIG_ESP_WIFI_CSI_ENABLED)\n");
#endif

    /* 扫描显示可见的内容 */
    printf("  WiFi Scan:      扫描附近 APs...\n");
    wifi_scan_config_t scan_cfg = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };
    esp_wifi_scan_start(&scan_cfg, true);  /* blocking scan */

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    printf("  找到的 APs:      %d\n", ap_count);

    if (ap_count > 0) {
        uint16_t max_show = (ap_count > 10) ? 10 : ap_count;
        wifi_ap_record_t *ap_list = malloc(sizeof(wifi_ap_record_t) * max_show);
        if (ap_list) {
            esp_wifi_scan_get_ap_records(&max_show, ap_list);
            printf("  %-32s  信道  RSSI  认证\n", "  SSID");
            printf("  %-32s  --  ----  ----\n", "  ----");
            for (int i = 0; i < max_show; i++) {
                const char *auth_str = "OPEN";
                switch (ap_list[i].authmode) {
                    case WIFI_AUTH_WEP:          auth_str = "WEP"; break;
                    case WIFI_AUTH_WPA_PSK:       auth_str = "WPA"; break;
                    case WIFI_AUTH_WPA2_PSK:      auth_str = "WPA2"; break;
                    case WIFI_AUTH_WPA_WPA2_PSK:  auth_str = "WPA/2"; break;
                    case WIFI_AUTH_WPA3_PSK:      auth_str = "WPA3"; break;
                    case WIFI_AUTH_WPA2_WPA3_PSK: auth_str = "WPA2/3"; break;
                    default: break;
                }
                printf("    %-30s  %2d  %4d  %s\n",
                       (char *)ap_list[i].ssid,
                       ap_list[i].primary,
                       ap_list[i].rssi,
                       auth_str);
            }
            free(ap_list);
            if (ap_count > max_show)
                printf("    ... and %d more\n", ap_count - max_show);
        }
    }

    /* 支持WiFi模式 */
    printf("\n  支持的WiFi模式:\n");
    printf("    - STA  (客户端 / 客户端)\n");
    printf("    - AP   (接入点 / 软-AP)\n");
    printf("    - STA+AP (并发)\n");
    printf("    - 杂模式 (原始802.11 frame capture)\n");
    printf("    - ESP-NOW (点对点，无需路由)\n");
    printf("    - WiFi Aware / NAN (邻居感知)\n");

    esp_wifi_stop();
    esp_wifi_deinit();
}

static void probe_bluetooth(void)
{
    print_separator("蓝牙能力探查");

    esp_chip_info_t info;
    esp_chip_info(&info);

    if (info.features & CHIP_FEATURE_BLE) {
        printf("  BLE:            支持 (蓝牙 LE)\n");
        printf("    - GATT 服务器/客户端\n");
        printf("    - 广播与扫描\n");
        printf("    - 网格网络\n");
        printf("    - 长距离 (编码 PHY)\n");
        printf("    - 2 Mbps PHY\n");
    } else {
        printf("  BLE:            不支持此芯片\n");
    }

#if CONFIG_IDF_TARGET_ESP32C6
    if (info.features & CHIP_FEATURE_IEEE802154) {
        printf("  802.15.4:       Supported (Thread / Zigbee style MAC)\n");
    }
#endif

    if (info.features & CHIP_FEATURE_BT) {
        printf("  BT Classic:     支持 (A2DP, SPP, HFP)\n");
    } else {
        printf("  BT Classic:     不支持此芯片\n");
    }
}

static void probe_peripherals(void)
{
    print_separator("外设能力探查");
    
    printf("  GPIOs:          %d total\n", SOC_GPIO_PIN_COUNT);
    printf("  ADC:\n");
#if CONFIG_IDF_TARGET_ESP32C6
    printf("    - SAR ADC:    %d channels (12-bit, one controller)\n",
           (int)SOC_ADC_CHANNEL_NUM(0));
#else
    printf("    - ADC1:       %d channels (12-bit SAR)\n", SOC_ADC_CHANNEL_NUM(0));
    printf("    - ADC2:       %d channels (shared with WiFi)\n", SOC_ADC_CHANNEL_NUM(1));
#endif
    printf("  DAC:            不支持此芯片\n");
#if CONFIG_IDF_TARGET_ESP32S3
    printf("  Touch Sensors:  %d channels (capacitive)\n", PROBE_TOUCH_CHAN_NUM);
#elif CONFIG_IDF_TARGET_ESP32C6
    printf("  Touch Sensors:  不支持此芯片\n");
#endif
    printf("  SPI:            %d controllers\n", SOC_SPI_PERIPH_NUM);
#if CONFIG_IDF_TARGET_ESP32S3
    printf("                  (SPI2/SPI3 typical for user apps)\n");
#endif
    printf("  I2C:            %d controllers\n", (int)SOC_I2C_NUM);
    printf("  I2S:            %d controller(s) (audio/PDM/TDM)\n", PROBE_I2S_CTRL_NUM);
    printf("  UART:           %d controllers\n", (int)SOC_UART_NUM);
#if CONFIG_IDF_TARGET_ESP32S3
    printf("  USB:            USB-OTG 1.1 (Host & Device)\n");
    printf("  USB-Serial:     Built-in USB-JTAG/Serial (this console)\n");
#elif CONFIG_IDF_TARGET_ESP32C6
    printf("  USB:            No native USB-OTG (use SPI/USB bridge or off-chip PHY)\n");
    printf("  USB-Serial:     Built-in USB Serial/JTAG (this console)\n");
#endif
#if CONFIG_IDF_TARGET_ESP32S3
    printf("  TWAI (CAN):     1 controller (CAN 2.0B compatible)\n");
#elif CONFIG_IDF_TARGET_ESP32C6
    printf("  TWAI (CAN):     %d controller(s) (CAN 2.0B compatible)\n",
           (int)SOC_TWAI_CONTROLLER_NUM);
#endif
    printf("  RMT:            %d channels (IR/WS2812/NeoPixel)\n", PROBE_RMT_CHAN_NUM);
    printf("  LEDC (PWM):     %d channels\n", SOC_LEDC_CHANNEL_NUM);
    printf("  MCPWM:          %d group(s) (motor control)\n", PROBE_MCPWM_GROUPS);
    printf("  PCNT:           %d units (pulse counter / encoder)\n", PROBE_PCNT_UNITS);
#if CONFIG_IDF_TARGET_ESP32S3
    printf("  LCD:            Parallel 8/16-bit + SPI + I2C interfaces\n");
    printf("  Camera:         DVP 8/16-bit parallel interface\n");
    printf("  SDMMC:          SD/MMC host controller (1-bit / 4-bit)\n");
#elif CONFIG_IDF_TARGET_ESP32C6
    printf("  PARLIO:         Parallel TX/RX (e.g. LED matrix / custom buses)\n");
    printf("  Camera:         SPI / external bridge (no native DVP)\n");
    printf("  SDIO:           SDIO slave peripheral (see TRM for capabilities)\n");
#endif
}

static void probe_security(void)
{
    print_separator("SECURITY & CRYPTO");

    printf("  AES:            128/256位硬件加速器支持\n");
    printf("  SHA:            SHA-1/224/256位硬件加速器支持\n");
    printf("  RSA:            4096位硬件加速器支持\n");
    printf("  HMAC:           支持\n");
    printf("  Digital Sig:    支持\n");
    printf("  Flash Encrypt:  AES-256-XTS (eFuse controlled)\n");
    printf("  Secure Boot:    V2 (RSA-3072 / ECDSA)\n");
    printf("  eFuse:          %d bits (MAC, keys, config)\n", 256 * 11);
    printf("  World Ctrl:     双世界隔离支持 (TEE)\n");
    printf("  Random:         支持\n");
}

static void probe_power(void)
{
    print_separator("电源管理探查");

#if CONFIG_IDF_TARGET_ESP32C6
    printf("  Clock Modes:\n");
    printf("    - 160 MHz     (max CPU on ESP32-C6)\n");
    printf("    - 120 MHz     (balanced)\n");
    printf("    - 80 MHz      (low power)\n");
#else
    printf("  Clock Modes:\n");
    printf("    - 240 MHz     (max performance)\n");
    printf("    - 160 MHz     (balanced)\n");
    printf("    - 80 MHz      (low power)\n");
#endif
    printf("  Sleep Modes:\n");
    printf("    - Modem Sleep  (WiFi off, CPU active)\n");
    printf("    - Light Sleep  (CPU paused, fast wake)\n");
    printf("    - Deep Sleep   (RTC only, ~10 uA)\n");
    printf("    - Hibernation  (RTC timer only, ~5 uA)\n");
#if CONFIG_IDF_TARGET_ESP32C6
    printf("  Wake Sources:   GPIO, LP timer, UART, etc.\n");
    printf("  LP domain:      LP core / LP peripherals (see TRM)\n");
#else
    printf("  Wake Sources:   GPIO, timer, touch, ULP, UART\n");
    printf("  ULP Coprocessor: FSM (runs in deep sleep)\n");
#endif
}
// 温度传感器探查
static void probe_temperature(void)
{
    print_separator("温度传感器探查");

    temperature_sensor_handle_t tsens = NULL;
    temperature_sensor_config_t tsens_cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);

    esp_err_t ret = temperature_sensor_install(&tsens_cfg, &tsens);
    if (ret == ESP_OK) {
        temperature_sensor_enable(tsens);
        float temp_c = 0;
        temperature_sensor_get_celsius(tsens, &temp_c);
        printf("  Chip Temp:      %.1f °C (%.1f °F)\n", temp_c, temp_c * 9.0 / 5.0 + 32.0);
        temperature_sensor_disable(tsens);
        temperature_sensor_uninstall(tsens);
    } else {
        printf("  Chip Temp:      未安装 (%s)\n", esp_err_to_name(ret));
    }
}
// FreeRTOS 系统探查
static void probe_freertos(void)
{
    print_separator("FreeRTOS 系统探查");

    printf("  FreeRTOS:       v%s\n", tskKERNEL_VERSION_NUMBER);
    printf("  Tick Rate:      %d Hz\n", configTICK_RATE_HZ);
    printf("  Task Count:     %"PRIu32"\n", (uint32_t)uxTaskGetNumberOfTasks());
    printf("  Main Stack:     %d bytes\n", CONFIG_ESP_MAIN_TASK_STACK_SIZE);
    printf("  Uptime:         %lld ms\n", esp_timer_get_time() / 1000LL);
}
// CSI 详情探查
static void probe_csi_details(void)
{
    print_separator("CSI (通道状态信息) 详情探查");

#ifdef CONFIG_ESP_WIFI_CSI_ENABLED
    printf("  Status:         ENABLED\n");
    printf("\n  什么是 CSI？\n");
    printf("    WiFi CSI captures the amplitude and phase of each OFDM\n");
    printf("    subcarrier in received WiFi frames. This gives a detailed\n");
    printf("    view of how radio signals propagate through a space.\n");
    printf("\n  Subcarriers:    52 (20 MHz) / 114 (40 MHz) per frame\n");
    printf("  Data Rate:      Up to ~100 frames/sec\n");
    printf("  Data per Frame: ~200-500 bytes (amplitude + phase)\n");
    printf("\n  Applications:\n");
    printf("    1. Presence Detection    — detect humans in a room\n");
    printf("    2. Gesture Recognition   — classify hand gestures\n");
    printf("    3. Activity Recognition  — walking, sitting, falling\n");
    printf("    4. Breathing/Heart Rate  — contactless vital signs\n");
    printf("    5. Indoor Positioning    — sub-meter localization\n");
    printf("    6. Fall Detection        — elderly safety monitoring\n");
    printf("    7. People Counting       — crowd estimation\n");
    printf("    8. Sleep Monitoring      — non-contact sleep staging\n");
    printf("\n  How to use:\n");
    printf("    esp_wifi_set_csi_config(&csi_config);\n");
    printf("    esp_wifi_set_csi_rx_cb(my_callback, NULL);\n");
    printf("    esp_wifi_set_csi(true);\n");
#else
    printf("  Status:         DISABLED\n");
    printf("  To enable:      Set CONFIG_ESP_WIFI_CSI_ENABLED=y in sdkconfig\n");
#endif
}

/* ── Main ────────────────────────────────────────────────────────────── */

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    /* NVS required for WiFi */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ── Hello World! ── */
    printf("\n");
    printf("  ╭─────────────────────────────────────────────────╮\n");
    printf("  │                                                 │\n");
    printf("  │       你好 from %-24s       │\n", chip_model_str(chip.model));
    printf("  │                                                 │\n");
    printf("  │   WiFi-DensePose 能力发现 v1.0      │\n");
    printf("  │                                                 │\n");
    printf("  ╰─────────────────────────────────────────────────╯\n");
    printf("\n");

    /* Run all probes */
    probe_chip_info();  // 探查芯片信息
    probe_memory();     // 探查内存
    probe_flash();  // 探查闪存
    probe_temperature();  // 探查温度
    probe_peripherals();  // 探查外设
    probe_security();   // 探查安全
    probe_power();      // 探查电源
    probe_freertos();  // 探查 FreeRTOS 系统
    probe_wifi_capabilities();  // 探查 WiFi 能力
    probe_bluetooth();  // 探查蓝牙
    probe_csi_details();  // 探查 CSI 详情

    print_separator("DONE — ALL CAPABILITIES REPORTED");
    printf("\n  这个 %s 已准备好 WiFi-DensePose 实验。\n",
           chip_model_str(chip.model));
    printf("  为了生产 CSI，请烧录 esp32-csi-node；路径 C6 路能不同。\n\n");

    /* Keep alive — 10 秒 更新一次状态消息 */
    int tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        tick++;
        printf("[hello] 仍在运行中... uptime=%lld 秒, 内存可用=%"PRIu32"\n",
               esp_timer_get_time() / 1000000LL,
               (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
}
