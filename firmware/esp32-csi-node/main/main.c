/**
 * @file main.c
 * @brief ESP32-S3 CSI Node — ADR-018 compliant firmware.
 *ESP32-S3 CSI Node - ADR-018兼容固件.
 * Initializes NVS, WiFi STA mode, CSI collection, and UDP streaming.
 * CSI frames are serialized in ADR-018 binary format and sent to the
 * aggregator over UDP.
 * 初始化NVS，WiFi STA模式，CSI采集，UDP流。
 * CSI帧被序列化为ADR-018二进制格式，并通过UDP发送到聚合器。
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_app_desc.h"
#include "sdkconfig.h"
#include "led_strip.h"

#include "csi_collector.h"
#include "stream_sender.h"
#include "nvs_config.h"
#include "edge_processing.h"
#include "ota_update.h"
#include "power_mgmt.h"
#include "wasm_runtime.h"
#include "wasm_upload.h"
#include "display_task.h"
#include "mmwave_sensor.h"
#include "swarm_bridge.h"
#include "rv_radio_ops.h"          /* ADR-081 Layer 1 — Radio Abstraction Layer. */
#include "adaptive_controller.h"   /* ADR-081 Layer 2 — Adaptive controller. */
#include "c6_twt.h"                /* ADR-110: TWT (no-op stub on S3) */
#include "c6_timesync.h"           /* ADR-110: 802.15.4 mesh time-sync (no-op on S3) */
#include "c6_lp_core.h"            /* ADR-110: LP-core hibernation (no-op on S3) */
#include "c6_sync_espnow.h"        /* ADR-110 D1 workaround: ESP-NOW sync */
#include "c6_softap_he.h"          /* ADR-110 B1/B2: HE/TWT soft-AP (no-op when disabled) */
#ifdef CONFIG_CSI_MOCK_ENABLED
#include "mock_csi.h"
#endif

#include "esp_timer.h"

static const char *TAG = "main";

/* ADR-040: WASM timer handle (calls on_timer at configurable interval). */
static esp_timer_handle_t s_wasm_timer;

/* Runtime configuration (loaded from NVS or Kconfig defaults).
 * Global so other modules (wasm_upload.c) can access pubkey, etc. */
nvs_config_t g_nvs_config;

/* Event group bits */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
#define MAX_RETRY 10

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "WiFi disconnected, reason=%d rssi=%d", disc->reason, disc->rssi);
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying WiFi connection (%d/%d)", s_retry_num, MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            /* WPA_PSK (not WPA2_PSK) so routers running WPA/WPA2-mixed
             * compatibility mode aren't rejected with
             * WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD (#1050). */
            .threshold.authmode = WIFI_AUTH_WPA_PSK,
        },
    };

    /* Copy runtime SSID/password from NVS config */
    strlcpy((char *)wifi_config.sta.ssid, g_nvs_config.wifi_ssid,
            sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, g_nvs_config.wifi_password,
            sizeof(wifi_config.sta.password));

    /* If password is empty, use open auth */
    if (strlen((char *)wifi_config.sta.password) == 0) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

#if defined(CONFIG_IDF_TARGET_ESP32C6) && defined(CONFIG_C6_SOFTAP_HE_ENABLE)
    /* ADR-110 B1/B2 cheap-unblock: bring up a soft-AP that advertises HE +
     * TWT Responder=1 so a second C6 board can negotiate iTWT against
     * this node. c6_softap_he_start() switches the mode to AP+STA. */
    uint8_t softap_chan = 0;
    if (c6_softap_he_start(&softap_chan) == ESP_OK) {
        ESP_LOGI(TAG, "C6 soft-AP HE armed on channel %u (ADR-110 B1/B2)", softap_chan);
    }
#endif

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA initialized, connecting to SSID: %s", g_nvs_config.wifi_ssid);

    /* Wait for connection */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to WiFi");
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to WiFi after %d retries", MAX_RETRY);
    }
}

#if CONFIG_LED_GAMMA_VIZ
/* Viridis colormap (60 steps), generated from ruv-neural-viz::ColorMap::viridis()
 * — the rUv-Neural brain-topology colormap, now no_std (ruvnet/ruv-neural#3 /
 * RuView#1126). Used as the ON-phase colour of the 40 Hz gamma flicker below:
 * dark-purple (still) -> teal -> green -> yellow (strong motion). */
static const uint8_t VIRIDIS_LUT[60][3] = {
    { 68,  1, 84},{ 67,  6, 88},{ 67, 12, 91},{ 66, 17, 95},{ 66, 23, 99},
    { 65, 28,103},{ 64, 34,106},{ 64, 39,110},{ 63, 45,114},{ 63, 50,118},
    { 62, 56,121},{ 61, 61,125},{ 61, 67,129},{ 60, 72,132},{ 59, 78,136},
    { 59, 83,139},{ 57, 87,139},{ 55, 92,139},{ 53, 96,139},{ 52,100,139},
    { 50,104,139},{ 48,109,139},{ 46,113,139},{ 44,117,140},{ 43,122,140},
    { 41,126,140},{ 39,130,140},{ 37,134,140},{ 36,139,140},{ 34,143,140},
    { 35,147,139},{ 39,151,136},{ 43,154,133},{ 47,158,130},{ 52,162,127},
    { 56,166,124},{ 60,170,121},{ 64,173,119},{ 68,177,116},{ 72,181,113},
    { 76,185,110},{ 81,189,107},{ 85,192,104},{ 89,196,102},{ 93,200, 99},
    {102,203, 95},{113,205, 91},{124,207, 87},{134,209, 82},{145,211, 78},
    {156,213, 74},{167,215, 70},{178,217, 66},{188,219, 62},{199,221, 58},
    {210,223, 54},{221,225, 49},{231,227, 45},{242,229, 41},{253,231, 37},
};
static led_strip_handle_t s_viz_led;

/* motion_energy that saturates the colormap to yellow (CONFIG, milli-units). */
#define LED_MOTION_FULLSCALE ((float)CONFIG_LED_MOTION_FULLSCALE_MILLI / 1000.0f)

/* GENUS-style 40 Hz gamma flicker: full on/off square wave, 50% duty (toggled
 * every 12.5 ms → 40 Hz). The ON colour is live CSI motion (edge motion_energy)
 * mapped through the ruv-neural-viz viridis LUT — still=purple, moving=yellow.
 * So the LED is a real 40 Hz gamma stimulus whose hue tracks sensed motion. */
static void led_gamma_40hz_cb(void *arg)
{
    static bool on = false;
    on = !on;
    if (on) {
        edge_vitals_pkt_t v;
        float m = edge_get_vitals(&v) ? v.motion_energy : 0.0f;
        float norm = m / LED_MOTION_FULLSCALE;
        if (norm < 0.0f) norm = 0.0f;
        if (norm > 1.0f) norm = 1.0f;
        int idx = (int)(norm * 59.0f + 0.5f);
        const uint8_t *c = VIRIDIS_LUT[idx];
        led_strip_set_pixel(s_viz_led, 0, c[0], c[1], c[2]); /* R,G,B (driver maps to GRB) */
    } else {
        led_strip_set_pixel(s_viz_led, 0, 0, 0, 0);          /* off phase */
    }
    led_strip_refresh(s_viz_led);
}
#endif /* CONFIG_LED_GAMMA_VIZ */

void app_main(void)
{
    /* 初始化NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 加载NVS配置 */
    nvs_config_load(&g_nvs_config);

    /* Capture node_id IMMEDIATELY — before wifi_init_sta() can corrupt
     * g_nvs_config. See #232/#375/#390: WiFi driver init clobbers the struct
     * on some devices, reverting node_id to the Kconfig default of 1. 
     *  WiFi驱动程序init破坏结构体
     * 在某些设备上，将node_id恢复为Kconfig默认值1。
     * 初始化CSI采集器节点ID,在WiFi STA模式下使用。
     */
    csi_collector_set_node_id(g_nvs_config.node_id);

    const esp_app_desc_t *app_desc = esp_app_get_description();
#if defined(CONFIG_IDF_TARGET_ESP32C6)
    const char *target_name = "ESP32-C6";
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
    const char *target_name = "ESP32-S3";
#else
    const char *target_name = "ESP32";
#endif
    ESP_LOGI(TAG, "%s CSI Node (ADR-018 / ADR-110) — v%s — Node ID: %d",
             target_name, app_desc->version, g_nvs_config.node_id); //返回esp_app_desc结构。这个结构包括应用版本。

    /* Onboard WS2812. C6 wires the LED to GPIO 8; S3 to GPIO 38 (DevKitC-1 v1.0)
     * or GPIO 48 (DevKitC-1 v1.1 / N16R8 — see #962). On S3 we drive 48 (the
     * common module). On C6, GPIO 38/48 don't exist (only 0-30) — gate by target.
     * Behaviour is set by CONFIG_LED_GAMMA_VIZ (ADR-183): on = 40 Hz gamma flicker
     * coloured by CSI motion; off = clear the LED at boot.
     WS2812 芯片的 C6 信号线将 LED 连接到 GPIO 8；S3 信号线连接到 GPIO 38（DevKitC-1 v1.0）或 GPIO 48（DevKitC-1 v1.1 / N16R8 — 参见 #962）。在 S3 上，我们驱动 48（即公共模块）。
    在 C6 上，GPIO 38/48 并不存在（仅支持 0-30），需根据目标设备进行门控。  
    行为由 CONFIG_LED_GAMMA_VIZ 控制（ADR-183）：开启时，LED 以 40 Hz 的伽马闪烁频率，通过 CSI 运动颜色变化显示；关闭时，在启动时清空 LED 灯光。
     */
#if defined(CONFIG_IDF_TARGET_ESP32C6)
    const int led_gpio = 8;
#else
    const int led_gpio = 48;
#endif
    // 配置LED条
    led_strip_config_t strip_config = {
        .strip_gpio_num = led_gpio,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    // 配置RMT通道
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .flags.with_dma = false,
    };
    // 如果LED可视化使能
#if CONFIG_LED_GAMMA_VIZ
    if (led_strip_new_rmt_device(&strip_config, &rmt_config, &s_viz_led) == ESP_OK) {
        const esp_timer_create_args_t viz_args = {
            .callback = &led_gamma_40hz_cb,
            .name = "led_gamma_40hz",
        };
        esp_timer_handle_t viz_timer;
        if (esp_timer_create(&viz_args, &viz_timer) == ESP_OK) {
            esp_timer_start_periodic(viz_timer, 12500); // 12.5 ms toggle → 40 Hz square wave
            ESP_LOGI(TAG, "Onboard WS2812: 40 Hz gamma flicker (GENUS), colour=CSI motion via ruv-neural-viz, GPIO %d", led_gpio);
        }
    }
#else
    /* Viz disabled — clear the onboard LED at boot and release the RMT channel. 
       Viz 已禁用 — 启动时清除板载 LED 并释放 RMT 通道。*/
    led_strip_handle_t led_strip;
    if (led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip) == ESP_OK) {
        led_strip_clear(led_strip);
        led_strip_del(led_strip);
    }
#endif /* CONFIG_LED_GAMMA_VIZ */

    /* ADR-110 P4: 802.15.4 mesh time-sync (C6 only).
     * Initialized BEFORE WiFi so it's available even when WiFi STA can't
     * connect — the radios are physically independent on the C6.
     * No-op on S3 (the helper compiles to an empty inline stub). 
     ADR-110 P4：802.15.4 网络时间同步（仅限C6）。
* 在WiFi初始化之前启动，因此即使WiFi STA无法连接时仍可用——C6上的无线电在物理上是独立的。
* 在S3模式下无操作（辅助组件编译为一个空的内联子程序）。*/
#if defined(CONFIG_IDF_TARGET_ESP32C6) && defined(CONFIG_C6_TIMESYNC_ENABLE)
    esp_err_t ts_ret = c6_timesync_init(CONFIG_C6_TIMESYNC_CHANNEL);
    if (ts_ret != ESP_OK) {
        ESP_LOGW(TAG, "c6_timesync_init failed: %s (continuing without 15.4 sync)",
                 esp_err_to_name(ts_ret));
    }
#endif

    /* ADR-110 P5: Optionally arm LP-core wake-on-motion (C6 only, opt-in).
     * Default off — only nodes flashed for battery-powered seed duty enable
     * this in menuconfig. 
     ADR-110 P5：可选启用LP核心唤醒功能（仅C6支持，需主动开启）。  
* 默认关闭 — 仅在电池供电的种子任务中启用闪存节点  
* 此设置位于菜单配置中。
     */
#if defined(CONFIG_IDF_TARGET_ESP32C6) && defined(CONFIG_C6_LP_CORE_ENABLE)
    if (c6_lp_core_was_motion_wake()) {
        ESP_LOGI(TAG, "boot cause: LP-core motion wake (running CSI burst)");
    }
#endif

    /* Initialize WiFi STA (skip entirely under QEMU mock — no RF hardware) 
     * 初始化WiFi STA（在QEMU模拟模式下跳过初始化，因为没有RF硬件）
     */
#ifndef CONFIG_CSI_MOCK_SKIP_WIFI_CONNECT
    wifi_init_sta();
#else
    ESP_LOGI(TAG, "模拟CSI模式：跳过WiFi初始化");
#endif

    /* Initialize UDP sender with runtime target 
     * 初始化UDP发送器（在运行时目标IP和端口）
     */
#ifdef CONFIG_CSI_MOCK_SKIP_WIFI_CONNECT
    ESP_LOGI(TAG, "模拟CSI模式：跳过UDP发送器初始化");
#else
    if (stream_sender_init_with(g_nvs_config.target_ip, g_nvs_config.target_port) != 0) {
        ESP_LOGE(TAG, "UDP发送器初始化失败");
        return;
    }
#endif

    /* Initialize CSI collection
     * 初始化CSI采集器（在QEMU模拟模式下替换真实WiFi CSI）
     */
#ifdef CONFIG_CSI_MOCK_ENABLED
    /* ADR-061: Start mock CSI generator (replaces real WiFi CSI in QEMU) */
    esp_err_t mock_ret = mock_csi_init(CONFIG_CSI_MOCK_SCENARIO);
    if (mock_ret != ESP_OK) {
        ESP_LOGE(TAG, "Mock CSI init failed: %s", esp_err_to_name(mock_ret));
    } else {
        ESP_LOGI(TAG, "Mock CSI active (scenario=%d)", CONFIG_CSI_MOCK_SCENARIO);
    }
#else
    csi_collector_init();
    ESP_LOGI(TAG, "CSI采集器初始化完成");

    /* ADR-073: Start multi-frequency channel hopping if configured in NVS.
    如果在NVS中配置，启动多频信道跳频 */
    if (g_nvs_config.channel_hop_count > 1) {
        ESP_LOGI(TAG, "多频信道跳频: %u 信道, dwell=%lu ms",
                 (unsigned)g_nvs_config.channel_hop_count,
                 (unsigned long)g_nvs_config.dwell_ms);
        // 设置多频信道跳频表
        csi_collector_set_hop_table(
            g_nvs_config.channel_list,
            g_nvs_config.channel_hop_count,
            g_nvs_config.dwell_ms);
    }
#endif

    /* ADR-110 P3: Request TWT from the AP for deterministic CSI cadence.
     * No-op on S3 (the helper compiles to an empty inline stub). On C6
     * the AP may NACK — the helper logs and falls back to opportunistic.
     * Called only after WiFi STA connect (wifi_init_sta blocks until then). 
     ADR-110 P3：向AP申请确定的CSI节奏的TWT。
* S3上无操作（helper编译成一个空的内联存根）。在C6
AP可能会NACK——helper记录日志并返回到机会主义。
*仅在WiFi STA连接后调用（wifi_init_sta阻塞直到那时）。*/
#if defined(CONFIG_IDF_TARGET_ESP32C6) && defined(CONFIG_C6_TWT_ENABLE)
    c6_twt_setup_default();
#endif

    /* ADR-110 D1 workaround: ESP-NOW cross-node sync. Initialized after
     * WiFi STA connects (ESP-NOW needs the WiFi driver up). Works on
     * both S3 and C6 — replaces the broken 802.15.4 RX path in c6_timesync.
     * Skip on QEMU mock (no real WiFi → no ESP-NOW). 
     ADR-110 D1：ESP-NOW跨节点同步（在WiFi STA连接后初始化）。
* 仅在C6上工作（替换c6_timesync中的802.15.4接收路径）。
* 在QEMU模拟模式下跳过初始化（因为没有真实WiFi，所以没有ESP-NOW）。*/
#ifndef CONFIG_CSI_MOCK_SKIP_WIFI_CONNECT
    esp_err_t espnow_ret = c6_sync_espnow_init();
    if (espnow_ret != ESP_OK) {
        ESP_LOGW(TAG, "ESP-NOW同步初始化失败（继续运行）",
                 esp_err_to_name(espnow_ret));
    }
#endif

    /* ADR-039: Initialize edge processing pipeline. 
    初始化边缘处理管道（在QEMU模拟模式下替换真实边缘处理） */
    edge_config_t edge_cfg = {
        .tier              = g_nvs_config.edge_tier,  // 边缘处理等级
        .presence_thresh   = g_nvs_config.presence_thresh,  // 存在阈值
        .fall_thresh       = g_nvs_config.fall_thresh,  // 跌落阈值
        .vital_window      = g_nvs_config.vital_window,  // 生命窗口（单位：毫秒）
        .vital_interval_ms = g_nvs_config.vital_interval_ms,  // 生命窗口间隔（单位：毫秒）
        .top_k_count       = g_nvs_config.top_k_count,  // 保留前K个事件
        .power_duty        = g_nvs_config.power_duty,  // 功率占空比（单位：毫秒）
    };
    esp_err_t edge_ret = edge_processing_init(&edge_cfg);
    if (edge_ret != ESP_OK) {
        ESP_LOGW(TAG, "边缘处理初始化失败（继续运行）",
                 esp_err_to_name(edge_ret));
    }

    /* ADR-040: Initialize OTA update HTTP server 
    初始化OTA更新HTTP服务器（需要QEMU模拟模式下替换真实OTA服务器） 
    */
    httpd_handle_t ota_server = NULL;
#ifndef CONFIG_CSI_MOCK_SKIP_WIFI_CONNECT
    esp_err_t ota_ret = ota_update_init_ex(&ota_server);
    if (ota_ret != ESP_OK) {
        ESP_LOGW(TAG, "OTA更新HTTP服务器初始化失败（继续运行）",
                 esp_err_to_name(ota_ret));
    }
#else
    esp_err_t ota_ret = ESP_ERR_NOT_SUPPORTED;
    ESP_LOGI(TAG, "模拟CSI模式：跳过OTA服务器（无网络）");
#endif

    /* ADR-040: Initialize WASM programmable sensing runtime.
    初始化WASM可编程传感器运行时环境（在QEMU模拟模式下替换真实WASM可编程传感器运行时环境） */
    esp_err_t wasm_ret = wasm_runtime_init();
    if (wasm_ret != ESP_OK) {
        ESP_LOGW(TAG, "WASM可编程传感器运行时环境初始化失败（继续运行）",
                 esp_err_to_name(wasm_ret));
    } else {
        /* Register WASM upload endpoints on the OTA HTTP server. 
        注册WASM上传端点到OTA更新HTTP服务器上
        如果OTA更新HTTP服务器未初始化，则不注册 */
        if (ota_server != NULL) {
            wasm_upload_register(ota_server);
        }

        /* Start periodic timer for wasm_runtime_on_timer(). 
           开始WASM可编程传感器运行时环境定时器，用于周期性调用wasm_runtime_on_timer()函数
        */
        esp_timer_create_args_t timer_args = {
            .callback = (void (*)(void *))wasm_runtime_on_timer,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "wasm_timer",
        };
        esp_err_t timer_ret = esp_timer_create(&timer_args, &s_wasm_timer);
        if (timer_ret == ESP_OK) {
#ifdef CONFIG_WASM_TIMER_INTERVAL_MS
            uint64_t interval_us = (uint64_t)CONFIG_WASM_TIMER_INTERVAL_MS * 1000ULL;
#else
            uint64_t interval_us = 1000000ULL;  /* Default: 1 second. */
#endif
            esp_timer_start_periodic(s_wasm_timer, interval_us);// 开始WASM可编程传感器运行时环境定时器，用于周期性调用wasm_runtime_on_timer()函数
            ESP_LOGI(TAG, "WASM on_timer（）周期性的: %llu ms",
                     (unsigned long long)(interval_us / 1000));
        } else {
            ESP_LOGW(TAG, "WASM可编程传感器运行时环境定时器创建失败（继续运行）",
                     esp_err_to_name(timer_ret));
        }
    }

    /* ADR-063: Initialize mmWave sensor (auto-detect on UART). 
    初始化mmWave传感器（自动检测UART端口） */
    esp_err_t mmwave_ret = mmwave_sensor_init(-1, -1);  /* -1 = use default GPIO pins */
    if (mmwave_ret == ESP_OK) {
        mmwave_state_t mw;
        if (mmwave_sensor_get_state(&mw)) {
            ESP_LOGI(TAG, "mmWave传感器: %s (caps=0x%04x)",
                     mmwave_type_name(mw.type), mw.capabilities);
        }
    } else {
        ESP_LOGI(TAG, "未检测到mmWave传感器（仅CSI模式）");
    }

    /* ADR-066: Initialize swarm bridge to Cognitum Seed (if configured). 
    初始化Cognitum Seed集群桥接（如果配置了） 
    */
    esp_err_t swarm_ret = ESP_ERR_INVALID_ARG;
#ifndef CONFIG_CSI_MOCK_SKIP_WIFI_CONNECT
    if (g_nvs_config.seed_url[0] != '\0') {  //如果配置了Cognitum Seed种子URL
        swarm_config_t swarm_cfg = {
            .heartbeat_sec = g_nvs_config.swarm_heartbeat_sec,  // 心跳间隔（单位：秒）
            .ingest_sec    = g_nvs_config.swarm_ingest_sec,  // 数据采集间隔（单位：秒）
            .enabled       = 1,
        };
        strlcpy(swarm_cfg.seed_url, g_nvs_config.seed_url,
                sizeof(swarm_cfg.seed_url));
        strlcpy(swarm_cfg.seed_token, g_nvs_config.seed_token,
                sizeof(swarm_cfg.seed_token));
        strlcpy(swarm_cfg.zone_name, g_nvs_config.zone_name,
                sizeof(swarm_cfg.zone_name));
        swarm_ret = swarm_bridge_init(&swarm_cfg, csi_collector_get_node_id()); // 初始化Cognitum Seed集群桥接
        if (swarm_ret != ESP_OK) {
            ESP_LOGW(TAG, "Cognitum Seed集群桥接初始化失败: %s", esp_err_to_name(swarm_ret));
        }
    } else {
        ESP_LOGI(TAG, "未配置Cognitum Seed种子URL，跳过初始化");
    }
#else
    ESP_LOGI(TAG, "模拟CSI模式：跳过蜂群");
#endif

    /* ADR-081 Layer 1: register the active radio ops binding.
     * - Real hardware: ESP32 binding wrapping csi_collector + esp_wifi.
     * - QEMU / offline: mock binding wrapping mock_csi.c.
     * Either way, the layers above (adaptive controller, mesh plane,
     * feature extraction) address the radio through the same vtable —
     * this is the portability acceptance test in ADR-081. 
     ADR-081第1层：注册活动无线电操作绑定。
* -真实硬件：ESP32绑定包装csi_collector + esp_wifi。 - QEMU / offline: mock绑定包装mock_csi.c无论哪种方式，
上面的层(自适应控制器，网格平面，特征提取)地址无线电通过相同的虚表-这是ADR-081中的可移植性验收测试。*/
#ifdef CONFIG_CSI_MOCK_ENABLED
    rv_radio_ops_mock_register();
#else
    rv_radio_ops_esp32_register();  // 注册ESP32无线电操作绑定
#endif
    const rv_radio_ops_t *radio_ops = rv_radio_ops_get();  // 获取当前无线电操作绑定
    if (radio_ops != NULL && radio_ops->init != NULL) {
        radio_ops->init();
    }

    /* ADR-081 Layer 2: start the adaptive controller. NULL config → use
     * Kconfig defaults. Default policy is conservative: no channel
     * switching, no role change. Operators opt in via menuconfig. 
     ADR-081 第二层：启动自适应控制器。NULL 配置 → 使用  
* Kconfig 默认值。默认策略为保守模式：不切换通道，不改变角色。操作员通过 menuconfig 启用。
     */
    esp_err_t adapt_ret = adaptive_controller_init(NULL);
    if (adapt_ret != ESP_OK) {
        ESP_LOGW(TAG, "自适应控制器初始化失败: %s",
                 esp_err_to_name(adapt_ret));
    }

    /* Initialize power management. 
    初始化电源管理。
    */
    power_mgmt_init(g_nvs_config.power_duty);

    /* ADR-045: Start AMOLED display task (gracefully skips if no display). 
    启动AMOLED显示任务（优雅跳过，如果无显示）。
    */
#ifdef CONFIG_DISPLAY_ENABLE
    esp_err_t disp_ret = display_task_start();
    if (disp_ret != ESP_OK) {
        ESP_LOGW(TAG, "显示初始化返回: %s", esp_err_to_name(disp_ret));
    }
#endif

    /* RuView#893/#521: the MGMT-only promiscuous filter (set in
     * csi_collector_init as the #396 display-crash workaround) starves the CSI
     * callback on display-less boards — yield collapses to 0 pps and the node
     * looks dead despite being on the network. Now that the display probe has
     * run, boards with no AMOLED panel (no QSPI/SPI-flash cache contention)
     * upgrade the filter to capture DATA frames too, restoring CSI yield. 
     RuView#893/#521：仅在MGMT中使用的随意过滤器（在csi_collector_init中设置为#396显示崩溃的临时解决方案）
     导致无显示屏板上的CSI回调饥饿——Yield降至0 pps，节点尽管处于网络中却看起来已失效。现在显示探测已完成，
     对于没有AMOLED面板的板卡（无QSPI/SPI闪存缓存竞争），升级过滤器以捕获DATA帧，从而恢复CSI的Yield。*/
#ifdef CONFIG_DISPLAY_ENABLE
    bool has_display = display_is_active();   /* runtime panel probe result运行时面板探测结果 */
#else
    bool has_display = false;                 /* display support not compiled in 显示支持未编译进固件 */
#endif
    if (!has_display) {
        csi_collector_enable_data_capture();
    }

    ESP_LOGI(TAG, "CSI流式传输已激活 → %s:%d (edge_tier=%u, OTA=%s, WASM=%s, mmWave=%s, swarm=%s, adapt=%s)",
             g_nvs_config.target_ip, g_nvs_config.target_port,
             g_nvs_config.edge_tier,
             (ota_ret == ESP_OK) ? "ready" : "off",
             (wasm_ret == ESP_OK) ? "ready" : "off",
             (mmwave_ret == ESP_OK) ? "active" : "off",
             (swarm_ret == ESP_OK) ? g_nvs_config.seed_url : "off",
             (adapt_ret == ESP_OK) ? "on" : "off");

    /* Main loop — keep alive */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
