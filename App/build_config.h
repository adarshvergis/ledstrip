/**
 * @file build_config.h
 * @brief Central build-time configuration of the Music Reactive LED Strip firmware.
 *
 * Every tunable compile-time constant lives here. Run-time tunables (algorithm
 * parameters, network credentials, brightness, ...) are NOT here: they are stored
 * in flash by the configuration manager and edited from the host software.
 */
#ifndef BUILD_CONFIG_H
#define BUILD_CONFIG_H

/* ========================================================================== */
/*  Firmware identification                                                   */
/* ========================================================================== */

#define FW_VERSION_MAJOR            0       /**< Incompatible protocol/storage changes      */
#define FW_VERSION_MINOR            1       /**< New features, backwards compatible         */
#define FW_VERSION_PATCH            0       /**< Bug fixes only                             */

#define FW_STR_(x)                  #x
#define FW_STR(x)                   FW_STR_(x)

/** Firmware version string reported to the host, e.g. "0.1.0". */
#define FW_VERSION_STRING           FW_STR(FW_VERSION_MAJOR) "." FW_STR(FW_VERSION_MINOR) "." FW_STR(FW_VERSION_PATCH)

/** Build timestamp (compiler supplied). */
#define FW_BUILD_DATE_STRING        __DATE__ " " __TIME__

#define FW_PRODUCT_NAME             "Music Reactive LED Strip"
#define FW_HW_NAME                  "Seeed Xiao ESP32C3 + WS2812B x60 + INMP441"

/** Version of the host <-> device protocol (Docs/protocol.md). */
#define PROTOCOL_VERSION            2

/* ========================================================================== */
/*  Build variants                                                            */
/* ========================================================================== */

/**
 * INTERNAL_TEST_BUILD: development build. Enables the debug CLI and the self
 * diagnostics. Release builds are made by passing -DLEDFW_RELEASE_BUILD
 * (Scripts/build.sh --release), which leaves this macro undefined.
 */
#ifndef LEDFW_RELEASE_BUILD
#define INTERNAL_TEST_BUILD
#endif

#ifdef INTERNAL_TEST_BUILD
#define ENABLE_DEBUG_CLI            /**< Text command line over the network (CMD 0x40) */
#define ENABLE_SELF_DIAGNOSTICS     /**< Heap/stack/CPU/benchmark reporting (CMD 0x50) */
/** Debug host commands: settings incl. WiFi credentials (0x20-0x22), error list
 *  (0x60/0x61). Release builds only serve main view, calibration and firmware update. */
#define ENABLE_DEBUG_HOST
#endif

/** Alias used by the host software documentation. */
#ifdef ENABLE_DEBUG_CLI
#define ENABLE_CLI
#endif

/* Bitfield sent to the host in GET_INFO so it can show/hide UI tabs. */
#define BUILD_FLAG_INTERNAL_TEST    (1u << 0)
#define BUILD_FLAG_DEBUG_CLI        (1u << 1)
#define BUILD_FLAG_SELF_DIAG        (1u << 2)
#define BUILD_FLAG_DEBUG_HOST       (1u << 3)

/* ========================================================================== */
/*  Boot messages (printed on the USB console)                                */
/* ========================================================================== */

#define APP_BOOT_MSG_BANNER         "\r\n=== " FW_PRODUCT_NAME " ===\r\n"
#define APP_BOOT_MSG_VERSION        "FW " FW_VERSION_STRING " built " FW_BUILD_DATE_STRING "\r\n"
#define APP_BOOT_MSG_HW             "HW " FW_HW_NAME "\r\n"
#ifdef INTERNAL_TEST_BUILD
#define APP_BOOT_MSG_VARIANT        "Variant: INTERNAL TEST BUILD (CLI + diagnostics enabled)\r\n"
#else
#define APP_BOOT_MSG_VARIANT        "Variant: release\r\n"
#endif
#define APP_BOOT_MSG_READY          "System ready.\r\n"

/* ========================================================================== */
/*  Flash layout - must match partitions.csv                                  */
/* ========================================================================== */

#define FLASH_SIZE_BYTES            (4u * 1024u * 1024u)
#define BOOTLOADER_ADDR             0x00000000u   /**< ESP32-C3 2nd stage bootloader       */
#define PARTITION_TABLE_ADDR        0x00008000u
#define OTADATA_ADDR                0x0000D000u   /**< Selects the boot slot               */
#define APP_SLOT0_ADDR              0x00010000u   /**< ota_0                               */
#define APP_SLOT1_ADDR              0x001F0000u   /**< ota_1                               */
#define APP_SLOT_SIZE               0x001E0000u
#define CALIB_REGION_ADDR           0x003D0000u   /**< Calibration parameter store (A/B)   */
#define CALIB_REGION_SIZE           0x00008000u
#define SETTINGS_REGION_ADDR        0x003D8000u   /**< Device/network settings store (A/B) */
#define SETTINGS_REGION_SIZE        0x00008000u

#define CALIB_PARTITION_LABEL       "calib"
#define SETTINGS_PARTITION_LABEL    "settings"
#define CALIB_PARTITION_SUBTYPE     0x40
#define SETTINGS_PARTITION_SUBTYPE  0x41

/** Size of one config slot (one flash sector). Two slots per region (A/B). */
#define CFG_SLOT_SIZE               4096u

/* ========================================================================== */
/*  Pins (Seeed Xiao ESP32C3)                                                 */
/* ========================================================================== */

#define PIN_LED_DATA                2       /**< D0 -> WS2812B DIN                         */
#define PIN_MIC_SCK                 3       /**< D1 -> INMP441 SCK (bit clock)             */
#define PIN_MIC_WS                  4       /**< D2 -> INMP441 WS  (word select)           */
#define PIN_MIC_SD                  5       /**< D3 <- INMP441 SD  (data)                  */

/* ========================================================================== */
/*  Audio acquisition & processing                                            */
/* ========================================================================== */

/** Microphone sample rate [Hz]. 16 kHz covers the musically relevant band (<8 kHz). */
#define SAMPLE_RATE                 16000u

/**
 * Samples per processing block (= one DMA block). This is the main latency knob:
 * 64 samples @16 kHz = 4 ms. Must be a power of two, <= AUDIO_FFT_SIZE, and the
 * block period must exceed the LED frame time (~2.1 ms for 60 LEDs).
 */
#define AUDIO_HOP_SIZE              64u

/** Number of DMA blocks in the microphone ring buffer (time headroom = N * hop). */
#define AUDIO_DMA_BLOCKS            4u

/** FFT length used by the spectrum stage (power of two, 64..512). 256 -> 62.5 Hz bins. */
#define AUDIO_FFT_SIZE              256u
#define AUDIO_FFT_LOG2              8u

/** Number of logarithmic frequency bands produced by the spectrum stage. */
#define AUDIO_NUM_BANDS             12u

/**
 * Microphone channel. INMP441 with L/R tied to GND outputs on the LEFT slot.
 * 0 = left, 1 = right, 2 = auto-detect at start-up (slot with signal wins).
 */
#define MIC_CHANNEL_LEFT            0
#define MIC_CHANNEL_RIGHT           1
#define MIC_CHANNEL_AUTO            2
#define MIC_CHANNEL_DEFAULT         MIC_CHANNEL_AUTO

/** Blocks without any microphone signal before ERR_MIC_NO_SIGNAL is raised (~2 s). */
#define MIC_NO_SIGNAL_BLOCKS        (2u * SAMPLE_RATE / AUDIO_HOP_SIZE)

/** Max parameters a single processing stage may expose. */
#define STAGE_MAX_PARAMS            12u

/** Max stages in one algorithm pipeline. */
#define PIPELINE_MAX_STAGES         12u

/* ========================================================================== */
/*  Algorithms                                                                */
/* ========================================================================== */

#define ALGO_AMPLITUDE_ALL          0       /**< All LEDs follow the amplitude envelope            */
#define ALGO_BEAT_ALL               1       /**< All LEDs flash on the detected main beat          */
#define ALGO_SPECTRUM_BARS          2       /**< Amplitude per frequency band -> bar per band      */
#define ALGO_BEAT_SEGMENTS          3       /**< Beat pulses positioned by spectral centroid       */

/** Number of algorithms compiled in (checked against the algorithm table). */
#define NUM_ALGORITHMS              4

/** Algorithm used on first boot / after a settings factory reset. */
#define DEFAULT_ALGORITHM           ALGO_BEAT_ALL
/** Spelling used in the original specification. */
#define DEFAULT_LGORITHM            DEFAULT_ALGORITHM

/* ========================================================================== */
/*  LED strip                                                                 */
/* ========================================================================== */

#define LED_MAX_COUNT               60u     /**< Physical LEDs (buffers are sized for this)  */
#define LED_DEFAULT_COUNT           60u
#define LED_DEFAULT_BRIGHTNESS      160u    /**< 0..255 global brightness                    */
/** Default current budget [mA]. 500 mA = safe on any USB port. */
#define LED_DEFAULT_MAX_CURRENT_MA  500u
#define LED_MA_PER_CHANNEL          20u     /**< WS2812B current per colour at full duty     */
#define LED_IDLE_MA_PER_LED         1u      /**< Quiescent current per LED                   */
#define LED_PATTERN_FRAME_MS        10u     /**< Frame period in pattern / system modes      */

/* ========================================================================== */
/*  Network                                                                   */
/* ========================================================================== */

#define NET_TCP_PORT                4210    /**< Command channel (host software)             */
#define NET_UDP_DISCOVERY_PORT      4211    /**< Discovery request / beacon port             */
#define NET_DISCOVERY_REQUEST       "LEDSTRIP_DISCOVER"
#define NET_BEACON_PERIOD_MS        3000
#define NET_MAX_PAYLOAD             4160    /**< Max frame payload (fits a 4 KiB data chunk) */
#define NET_FW_CHUNK_SIZE           4096    /**< FW_DATA chunk size advertised to the host   */
#define NET_RX_IDLE_TIMEOUT_S       120     /**< Drop an idle client after this time         */

#define NET_DEFAULT_DEVICE_NAME     "ledstrip"
/* Station only (no soft-AP): credentials come from the settings store, written by the
 * host in debug builds or by Scripts/flash_settings.sh. No SSID -> WiFi stays off. */
#define NET_STA_RETRY_MIN_MS        1000    /**< First reconnect delay after a disconnect    */
#define NET_STA_RETRY_MAX_MS        10000   /**< Reconnect back-off limit                    */

/* Live stream (CMD_LIVE_SUBSCRIBE): UDP datagrams pushed to the host, latest snapshot only. */
#define NET_LIVE_DEFAULT_HZ         60
#define NET_LIVE_MAX_HZ             100
#define NET_LIVE_LEASE_MS           3000    /**< Stream stops without host traffic this long  */

/* ========================================================================== */
/*  RTOS tasks                                                                */
/* ========================================================================== */

/* Real-time task (mic -> pipeline -> LEDs) runs above the WiFi task (23) for minimum
 * latency. It blocks on the DMA interrupt and only uses ~10-25 % CPU. */
#define TASK_PRIO_RT                24
#define TASK_PRIO_COMM              5
#define TASK_PRIO_DISCOVERY         3
#define TASK_PRIO_SUPERVISOR        2

#define TASK_STACK_RT               6144
#define TASK_STACK_COMM             6144
#define TASK_STACK_DISCOVERY        3072

/* ========================================================================== */
/*  Error handling / watchdog                                                 */
/* ========================================================================== */

#define WDT_HW_TIMEOUT_MS           3000    /**< Hardware MWDT (TG0) reset timeout          */
#define WDT_SUPERVISOR_PERIOD_MS    100     /**< Supervisor checks clients at this rate     */
#define WDT_RT_TASK_TIMEOUT_MS      1000    /**< Real-time task must check in within this   */
#define WDT_COMM_TASK_TIMEOUT_MS    10000   /**< Comm task must check in within this        */

#define ERR_FATAL_REBOOT_DELAY_MS   8000    /**< Show fatal error pattern, then reboot      */
#define ERR_WARNING_DISPLAY_MS      4000    /**< Warnings are shown on the strip this long  */
#define OTA_CONFIRM_UPTIME_MS       10000   /**< Healthy uptime before new FW is confirmed  */
#define HEAP_LOW_WARN_BYTES         12000
#define STACK_LOW_WARN_BYTES        512

#endif /* BUILD_CONFIG_H */
