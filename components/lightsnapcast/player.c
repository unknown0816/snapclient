/**
 *
 */

#include <sys/time.h>
#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "soc/rtc.h"


#include "driver/timer.h"

#include "MedianFilter.h"
#include "board_pins_config.h"
#include "player.h"
#include "snapcast.h"

#include "i2s.h"	// use custom i2s driver instead of IDF version

#define SYNC_TASK_PRIORITY 20
#define SYNC_TASK_CORE_ID tskNO_AFFINITY

static const char *TAG = "PLAYER";

/**
 * @brief Pre define APLL parameters, save compute time. They are calculated in player_setup_i2s()
 *        | bits_per_sample | rate | sdm0 | sdm1 | sdm2 | odir
 *
 *        apll_freq = xtal_freq * (4 + sdm2 + sdm1/256 + sdm0/65536)/((o_div +
 * 2) * 2) I2S bit clock is (apll_freq / 16)
 */
static int apll_normal_predefine[6] = {0, 0, 0, 0, 0, 0};
static int apll_corr_predefine[][6] = {{0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}};

static SemaphoreHandle_t latencyBufSemaphoreHandle = NULL;

static int8_t latencyBuffFull = 0;

static sMedianFilter_t latencyMedianFilterLong;
static sMedianNode_t latencyMedianLongBuffer[LATENCY_MEDIAN_FILTER_LEN];

static int64_t latencyToServer = 0;

static sMedianFilter_t shortMedianFilter;
static sMedianNode_t shortMedianBuffer[SHORT_BUFFER_LEN];

static int8_t currentDir = 0;  //!< current apll direction, see apll_adjust()

static QueueHandle_t pcmChunkQueueHandle = NULL;
#define PCM_CHNK_QUEUE_LENGTH \
  50  // TODO: one chunk is hardcoded to 20ms, change it to be dynamically
       // adjustable.
static StaticQueue_t pcmChunkQueue;
static uint8_t pcmChunkQueueStorageArea[PCM_CHNK_QUEUE_LENGTH *
                                        sizeof(pcm_chunk_message_t *)];

static TaskHandle_t syncTaskHandle = NULL;

static QueueHandle_t snapcastSettingQueueHandle = NULL;

static size_t chunkInBytes;

static uint32_t i2sDmaBufCnt;
static uint32_t i2sDmaBufMaxLen;

snapcastSetting_t currentSnapcastSetting;

static void tg0_timer_init(void);
static void tg0_timer_deinit(void);
static void snapcast_sync_task(void *pvParameters);

/*
#define CONFIG_MASTER_I2S_BCK_PIN 5
#define CONFIG_MASTER_I2S_LRCK_PIN 25
#define CONFIG_MASTER_I2S_DATAOUT_PIN 26
#define CONFIG_SLAVE_I2S_BCK_PIN 26
#define CONFIG_SLAVE_I2S_LRCK_PIN 12
#define CONFIG_SLAVE_I2S_DATAOUT_PIN 5
*/

static esp_err_t player_setup_i2s(i2s_port_t i2sNum, snapcastSetting_t *setting) {
  int chunkInFrames;
  int __dmaBufCnt;
  int __dmaBufLen;
  const int __dmaBufMaxLen = 1024;
  int m_scale = 8, fi2s_clk;

  chunkInBytes = (setting->chunkDuration_ms * setting->sampleRate * setting->channels * (setting->bits / 8)) / 1000;
  chunkInFrames = chunkInBytes / (setting->channels * (setting->bits / 8));

  __dmaBufCnt = 1;
  __dmaBufLen = chunkInFrames;
  while ((__dmaBufLen >= __dmaBufMaxLen) || (__dmaBufCnt <= 1)) {
    if ((__dmaBufLen % 2) == 0) {
      __dmaBufCnt *= 2;
      __dmaBufLen /= 2;
    } else {
      ESP_LOGE(TAG,
               "player_setup_i2s: Can't setup i2s with this configuration");

      return -1;
    }
  }

  i2sDmaBufCnt = __dmaBufCnt;
  i2sDmaBufMaxLen = __dmaBufLen;

  fi2s_clk = setting->sampleRate * setting->channels * setting->bits * m_scale;

  apll_normal_predefine[0] = setting->bits;
  apll_normal_predefine[1] = setting->sampleRate;
  if (i2s_apll_calculate_fi2s(fi2s_clk, setting->bits, &apll_normal_predefine[2], &apll_normal_predefine[3], &apll_normal_predefine[4], &apll_normal_predefine[5]) != ESP_OK) {
	  ESP_LOGE(TAG, "ERROR, fi2s_clk");
  }

  apll_corr_predefine[0][0] = setting->bits;
  apll_corr_predefine[0][1] = setting->sampleRate * 1.001;
  if (i2s_apll_calculate_fi2s(fi2s_clk * 1.001, setting->bits, &apll_corr_predefine[0][2], &apll_corr_predefine[0][3], &apll_corr_predefine[0][4], &apll_corr_predefine[0][5]) != ESP_OK) {
 	  ESP_LOGE(TAG, "ERROR, fi2s_clk * 1.001");
   }
  apll_corr_predefine[1][0] = setting->bits;
  apll_corr_predefine[1][1] = setting->sampleRate * 0.999;
  if (i2s_apll_calculate_fi2s(fi2s_clk * 0.999, setting->bits, &apll_corr_predefine[1][2], &apll_corr_predefine[1][3], &apll_corr_predefine[1][4], &apll_corr_predefine[1][5]) != ESP_OK) {
	  ESP_LOGE(TAG, "ERROR, fi2s_clk * 0.999");
  }

  ESP_LOGI(TAG, "player_setup_i2s: dma_buf_len is %d, dma_buf_count is %d",
		  i2sDmaBufMaxLen, i2sDmaBufCnt);

  i2s_config_t i2s_config0 = {
      .mode = I2S_MODE_MASTER | I2S_MODE_TX,  // Only TX
      .sample_rate = setting->sampleRate,
      .bits_per_sample = setting->bits,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,  // 2-channels
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .dma_buf_count = i2sDmaBufCnt,
      .dma_buf_len = i2sDmaBufMaxLen,
      .intr_alloc_flags = 1,  // Default interrupt priority
      .use_apll = true,
      .fixed_mclk = 0,
      .tx_desc_auto_clear = true  // Auto clear tx descriptor on underflow
  };

  i2s_pin_config_t pin_config0;
  get_i2s_pins(i2sNum, &pin_config0);

  i2s_custom_driver_uninstall(i2sNum);
  i2s_custom_driver_install(i2sNum, &i2s_config0, 0, NULL);
  i2s_custom_set_pin(i2sNum, &pin_config0);

  return 0;
}

// ensure this is called after http_task was killed!
int deinit_player(void) {
  int ret = 0;

  pcm_chunk_message_t *chnk = NULL;

  // stop the task
  if (syncTaskHandle == NULL) {
    ESP_LOGW(TAG, "no sync task created?");
  } else {
    vTaskDelete(syncTaskHandle);
  }

  if (pcmChunkQueueHandle == NULL) {
    ESP_LOGW(TAG, "no pcm chunk queue created?");
  } else {
    // free all allocated memory
    while (uxQueueMessagesWaiting(pcmChunkQueueHandle)) {
      ret = xQueueReceive(pcmChunkQueueHandle, &chnk, pdMS_TO_TICKS(2000));
      if (ret != pdFAIL) {
        if (chnk != NULL) {
        	free_pcm_chunk(chnk);
        }
      }
    }

    // delete the queue
    vQueueDelete(pcmChunkQueueHandle);
    pcmChunkQueueHandle = NULL;
  }

  if (latencyBufSemaphoreHandle == NULL) {
    ESP_LOGW(TAG, "no latency buffer semaphore created?");
  } else {
    vSemaphoreDelete(latencyBufSemaphoreHandle);
    latencyBufSemaphoreHandle = NULL;
  }

  tg0_timer_deinit();

  ESP_LOGI(TAG, "deinit player done");

  return ret;
}

/**
 *  call before http task creation!
 */
QueueHandle_t init_player(void) {
  int ret;

  currentSnapcastSetting.buffer_ms = 1000;
  currentSnapcastSetting.chunkDuration_ms = 20;
  currentSnapcastSetting.codec = NONE;
  currentSnapcastSetting.sampleRate = 44100;
  currentSnapcastSetting.channels = 2;
  currentSnapcastSetting.bits = 16;
  currentSnapcastSetting.muted = false;
  currentSnapcastSetting.volume = 70;

  ret = player_setup_i2s(I2S_NUM_0, &currentSnapcastSetting);
  if (ret < 0) {
    ESP_LOGE(TAG, "player_setup_i2s failed: %d", ret);

    return NULL;
  }

  // create semaphore for time diff buffer to server
  if (latencyBufSemaphoreHandle == NULL) {
    latencyBufSemaphoreHandle = xSemaphoreCreateMutex();
  }

  // create snapcast receive buffer
  if (pcmChunkQueueHandle == NULL) {
	pcmChunkQueueHandle = xQueueCreateStatic(
		PCM_CHNK_QUEUE_LENGTH, sizeof(pcm_chunk_message_t *),
		pcmChunkQueueStorageArea, &pcmChunkQueue);
  }

  // init diff buff median filter
  latencyMedianFilterLong.numNodes = LATENCY_MEDIAN_FILTER_LEN;
  latencyMedianFilterLong.medianBuffer = latencyMedianLongBuffer;
  reset_latency_buffer();

  tg0_timer_init();

  if (syncTaskHandle == NULL) {
    ESP_LOGI(TAG, "Start snapcast_sync_task");

    xTaskCreatePinnedToCore(snapcast_sync_task, "snapcast_sync_task", 8 * 1024,
                            NULL, SYNC_TASK_PRIORITY, &syncTaskHandle,
                            SYNC_TASK_CORE_ID);
  }

  ESP_LOGI(TAG, "init player done");

  return pcmChunkQueueHandle;
}

int8_t player_latency_insert(int64_t newValue) {
  int64_t medianValue;

  medianValue = MEDIANFILTER_Insert(&latencyMedianFilterLong, newValue);
  if (xSemaphoreTake(latencyBufSemaphoreHandle, pdMS_TO_TICKS(1)) == pdTRUE) {
    if (MEDIANFILTER_isFull(&latencyMedianFilterLong)) {
      latencyBuffFull = true;
    }

    latencyToServer = medianValue;

    xSemaphoreGive(latencyBufSemaphoreHandle);
  } else {
    ESP_LOGW(TAG, "couldn't set latencyToServer = medianValue");
  }

  return 0;
}

/**
 *
 */
int8_t player_notify_buffer_ms(uint32_t ms) {
  if (syncTaskHandle == NULL) {
    return -1;
  }

  // notify task of changed parameters
  xTaskNotify(syncTaskHandle, ms, eSetBits);

  return 0;
}

/**
 *
 */
int8_t player_send_snapcast_setting(snapcastSetting_t setting) {
  int ret;

  if ((syncTaskHandle == NULL) || (snapcastSettingQueueHandle == NULL)) {
    return -1;
  }

  if ((currentSnapcastSetting.bits != setting.bits) ||
      (currentSnapcastSetting.buffer_ms != setting.buffer_ms) ||
	  (currentSnapcastSetting.channels != setting.channels) ||
	  (currentSnapcastSetting.chunkDuration_ms != setting.chunkDuration_ms) ||
	  (currentSnapcastSetting.codec != setting.codec) ||
	  (currentSnapcastSetting.muted != setting.muted) ||
	  (currentSnapcastSetting.sampleRate != setting.sampleRate) ||
	  (currentSnapcastSetting.volume != setting.volume))
  {
	  // notify task of changed parameters
	  memcpy(&currentSnapcastSetting, &setting, sizeof(snapcastSetting_t));
	  ret = xQueueSend(snapcastSettingQueueHandle, &currentSnapcastSetting, pdMS_TO_TICKS(10));
	  if (ret != pdPASS) {
		  ESP_LOGE(TAG, "player_send_snapcast_setting: couldn't send snapcast setting");
	  }
  }

  return 0;
}

/**
 *
 */
int8_t reset_latency_buffer(void) {
  // init diff buff median filter
  if (MEDIANFILTER_Init(&latencyMedianFilterLong) < 0) {
    ESP_LOGE(TAG, "reset_diff_buffer: couldn't init median filter long. STOP");

    return -2;
  }

  if (latencyBufSemaphoreHandle == NULL) {
    ESP_LOGE(TAG, "reset_diff_buffer: latencyBufSemaphoreHandle == NULL");

    return -2;
  }

  if (xSemaphoreTake(latencyBufSemaphoreHandle, portMAX_DELAY) == pdTRUE) {
    latencyBuffFull = false;
    latencyToServer = 0;

    xSemaphoreGive(latencyBufSemaphoreHandle);
  } else {
    ESP_LOGW(TAG, "reset_diff_buffer: can't take semaphore");

    return -1;
  }

  return 0;
}

/**
 *
 */
int8_t latency_buffer_full(void) {
  int8_t tmp;

  if (latencyBufSemaphoreHandle == NULL) {
    ESP_LOGE(TAG, "latency_buffer_full: latencyBufSemaphoreHandle == NULL");

    return -2;
  }

  if (xSemaphoreTake(latencyBufSemaphoreHandle, 0) == pdFALSE) {
    ESP_LOGW(TAG, "latency_buffer_full: can't take semaphore");

    return -1;
  }

  tmp = latencyBuffFull;

  xSemaphoreGive(latencyBufSemaphoreHandle);

  return tmp;
}

/**
 *
 */
int8_t get_diff_to_server(int64_t *tDiff) {
  static int64_t lastDiff = 0;

  if (latencyBufSemaphoreHandle == NULL) {
    ESP_LOGE(TAG, "get_diff_to_server: latencyBufSemaphoreHandle == NULL");

    return -2;
  }

  if (xSemaphoreTake(latencyBufSemaphoreHandle, 0) == pdFALSE) {
    *tDiff = lastDiff;

    // ESP_LOGW(TAG, "get_diff_to_server: can't take semaphore. Old diff
    // retrieved");

    return -1;
  }

  *tDiff = latencyToServer;
  lastDiff = latencyToServer;  // store value, so we can return a value if
                               // semaphore couldn't be taken

  xSemaphoreGive(latencyBufSemaphoreHandle);

  return 0;
}

/**
 *
 */
int8_t server_now(int64_t *sNow) {
  struct timeval now;
  int64_t diff;

  // get current time
  if (gettimeofday(&now, NULL)) {
    ESP_LOGE(TAG, "server_now: Failed to get time of day");

    return -1;
  }

  if (get_diff_to_server(&diff) == -1) {
    ESP_LOGW(TAG,
             "server_now: can't get current diff to server. Retrieved old one");
  }

  if (diff == 0) {
    // ESP_LOGW(TAG, "server_now: diff to server not initialized yet");

    return -1;
  }

  *sNow = ((int64_t)now.tv_sec * 1000000LL + (int64_t)now.tv_usec) + diff;

  //	ESP_LOGI(TAG, "now: %lldus", (int64_t)now.tv_sec * 1000000LL +
  //(int64_t)now.tv_usec); 	ESP_LOGI(TAG, "diff: %lldus", diff);
  // ESP_LOGI(TAG, "serverNow: %lldus", *snow);

  return 0;
}

/*
 * Timer group0 ISR handler
 *
 * Note:
 * We don't call the timer API here because they are not declared with
 * IRAM_ATTR. If we're okay with the timer irq not being serviced while SPI
 * flash cache is disabled, we can allocate this interrupt without the
 * ESP_INTR_FLAG_IRAM flag and use the normal API.
 */
void IRAM_ATTR timer_group0_isr(void *para) {
  timer_spinlock_take(TIMER_GROUP_0);

  BaseType_t xHigherPriorityTaskWoken = pdFALSE;

  // Retrieve the interrupt status and the counter value
  //   from the timer that reported the interrupt
  uint32_t timer_intr = timer_group_get_intr_status_in_isr(TIMER_GROUP_0);

  // Clear the interrupt
  //   and update the alarm time for the timer with without reload
  if (timer_intr & TIMER_INTR_T1) {
    timer_group_clr_intr_status_in_isr(TIMER_GROUP_0, TIMER_1);

    // Notify the task in the task's notification value.
    xTaskNotifyFromISR(syncTaskHandle, 0, eNoAction, &xHigherPriorityTaskWoken);
  }

  timer_spinlock_give(TIMER_GROUP_0);

  if (xHigherPriorityTaskWoken) {
    portYIELD_FROM_ISR();
  }
}

static void tg0_timer_deinit(void) { timer_deinit(TIMER_GROUP_0, TIMER_1); }

/*
 *
 */
static void tg0_timer_init(void) {
  // Select and initialize basic parameters of the timer
  timer_config_t config = {
      //.divider = 8,		// 100ns ticks
      .divider = 80,  // 1µs ticks
      .counter_dir = TIMER_COUNT_UP,
      .counter_en = TIMER_PAUSE,
      .alarm_en = TIMER_ALARM_EN,
      .auto_reload = TIMER_AUTORELOAD_DIS,
  };  // default clock source is APB
  timer_init(TIMER_GROUP_0, TIMER_1, &config);

  // Configure the alarm value and the interrupt on alarm.
  timer_set_alarm_value(TIMER_GROUP_0, TIMER_1, 0);
  timer_enable_intr(TIMER_GROUP_0, TIMER_1);
  if (timer_isr_register(TIMER_GROUP_0, TIMER_1, timer_group0_isr, NULL,
                         ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3,
                         NULL) != ESP_OK) {
    ESP_LOGE(TAG, "unable to register timer 1 callback");
  }
}

/**
 *
 */
static void tg0_timer1_start(uint64_t alarm_value) {
  timer_pause(TIMER_GROUP_0, TIMER_1);
  timer_set_counter_value(TIMER_GROUP_0, TIMER_1, 0);
  timer_set_alarm_value(TIMER_GROUP_0, TIMER_1, alarm_value);
  timer_set_alarm(TIMER_GROUP_0, TIMER_1, TIMER_ALARM_EN);
  timer_start(TIMER_GROUP_0, TIMER_1);

  //    ESP_LOGI(TAG, "started age timer");
}

// void rtc_clk_apll_enable(bool enable, uint32_t sdm0, uint32_t sdm1, uint32_t
// sdm2, uint32_t o_div); apll_freq = xtal_freq * (4 + sdm2 + sdm1/256 +
// sdm0/65536)/((o_div + 2) * 2) xtal == 40MHz on lyrat v4.3 I2S bit_clock =
// rate * (number of channels) * bits_per_sample
void adjust_apll(int8_t direction) {
  int sdm0, sdm1, sdm2, o_div;
//  int index = 2;  // 2 for slow adjustment, 0 for fast adjustment

  // only change if necessary
  if (currentDir == direction) {
    return;
  }

//  if (direction == 1) {
//    // speed up
//    sdm0 = apll_predefine_48k_corr[index][2];
//    sdm1 = apll_predefine_48k_corr[index][3];
//    sdm2 = apll_predefine_48k_corr[index][4];
//    o_div = apll_predefine_48k_corr[index][5];
//  } else if (direction == -1) {
//    // slow down
//    sdm0 = apll_predefine_48k_corr[index + 1][2];
//    sdm1 = apll_predefine_48k_corr[index + 1][3];
//    sdm2 = apll_predefine_48k_corr[index + 1][4];
//    o_div = apll_predefine_48k_corr[index + 1][5];
//  } else {
//    // reset to normal playback speed
//    sdm0 = apll_predefine[5][2];
//    sdm1 = apll_predefine[5][3];
//    sdm2 = apll_predefine[5][4];
//    o_div = apll_predefine[5][5];
//
//    direction = 0;
//  }

  if (direction == 1) {
    // speed up
    sdm0 = apll_corr_predefine[0][2];
    sdm1 = apll_corr_predefine[0][3];
    sdm2 = apll_corr_predefine[0][4];
    o_div = apll_corr_predefine[0][5];
  } else if (direction == -1) {
    // slow down
    sdm0 = apll_corr_predefine[1][2];
    sdm1 = apll_corr_predefine[1][3];
    sdm2 = apll_corr_predefine[1][4];
    o_div = apll_corr_predefine[1][5];
  } else {
    // reset to normal playback speed
    sdm0 = apll_normal_predefine[2];
    sdm1 = apll_normal_predefine[3];
    sdm2 = apll_normal_predefine[4];
    o_div = apll_normal_predefine[5];

    direction = 0;
  }

  rtc_clk_apll_enable(1, sdm0, sdm1, sdm2, o_div);

  currentDir = direction;
}

/**
 *
 */
int8_t free_pcm_chunk_fragments(pcm_chunk_fragment_t *fragment) {
	if (fragment == NULL) {
		ESP_LOGE( TAG,
				"free_pcm_chunk_fragments() parameter Error");

		return -1;
	}

	// free all fragments recursive
	if (fragment->nextFragment == NULL) {
		if (fragment->payload != NULL) {
			free(fragment->payload);
			fragment->payload = NULL;
		}

		free(fragment);
		fragment = NULL;
	}
	else {
		free_pcm_chunk_fragments(fragment->nextFragment);
	}

	return 0;
}

/**
 *
 */
int8_t free_pcm_chunk(pcm_chunk_message_t *pcmChunk) {
	if (pcmChunk == NULL) {
		ESP_LOGE( TAG,
					"free_pcm_chunk() parameter Error");

		return -1;
	}

	free_pcm_chunk_fragments(pcmChunk->fragment);
	pcmChunk->fragment = NULL;	// was freed in free_pcm_chunk_fragments()

	free(pcmChunk);
	pcmChunk = NULL;

	return 0;
}


/**
 *
 */
int8_t insert_pcm_chunk(wire_chunk_message_t *decodedWireChunk) {
	pcm_chunk_message_t *pcmChunk;
	size_t tmpSize;
	size_t largestFreeBlock, freeMem;
	int ret = -3;

// 			heap_caps_get_free_size(MALLOC_CAP_8BIT);
//			heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
//          heap_caps_get_free_size(MALLOC_CAP_32BIT);
//			heap_caps_get_largest_free_block(MALLOC_CAP_32BIT);

	if (decodedWireChunk == NULL) {
		ESP_LOGE(
					TAG,
					"Parameter Error");

		return -1;
	}

	pcmChunk = (pcm_chunk_message_t *)calloc(1, sizeof(pcm_chunk_message_t));
	if (pcmChunk == NULL) {
		ESP_LOGE(
		TAG,
		"Failed to allocate memory for pcm chunk message");

		return -2;
	}

	pcmChunk->fragment = (pcm_chunk_fragment_t *)calloc(1, sizeof(pcm_chunk_fragment_t));
	if (pcmChunk->fragment == NULL) {
		ESP_LOGE(
				TAG,
				"Failed to allocate memory for pcm chunk fragment");

		free_pcm_chunk(pcmChunk);

		return -2;
	}

	// store the timestamp
	pcmChunk->timestamp = decodedWireChunk->timestamp;

#if CONFIG_USE_PSRAM
	pcmChunk->fragment->payload = (char *)heap_caps_malloc(decodedWireChunk->size, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
	if (pcmChunk->fragment->payload == NULL) {
		ESP_LOGE(
				TAG,
				"Failed to allocate memory for pcm chunk fragment payload");

		free_pcm_chunk(pcmChunk);

		freeMem = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);

		ret = -2;
	}
	else {
		// copy the whole payload to our fragment
		memcpy(pcmChunk->fragment->payload, decodedWireChunk->payload, decodedWireChunk->size);
		pcmChunk->fragment->nextFragment = NULL;
		pcmChunk->fragment->size = decodedWireChunk->size;

		ret = 0;
	}
#else
	// we got valid memory for pcm_chunk_message_t
	// first we try to allocated 32 bit aligned memory for payload
	// check available memory first so we can decide if we need to fragment the data
	freeMem = heap_caps_get_free_size(MALLOC_CAP_32BIT);
	if (freeMem >= decodedWireChunk->size) {
		largestFreeBlock = heap_caps_get_largest_free_block(MALLOC_CAP_32BIT);
//		ESP_LOGI(
//				TAG,
//				"32b f %d b %d", freeMem, largestFreeBlock);
		if (largestFreeBlock >= decodedWireChunk->size) {
			pcmChunk->fragment->payload = (char *)heap_caps_malloc(decodedWireChunk->size, MALLOC_CAP_32BIT);
			if (pcmChunk->fragment->payload == NULL) {
				ESP_LOGE(
						TAG,
						"Failed to allocate memory for pcm chunk fragment payload");

				free_pcm_chunk(pcmChunk);

				ret = -2;
			}
			else {
				// copy the whole payload to our fragment
				memcpy(pcmChunk->fragment->payload, decodedWireChunk->payload, decodedWireChunk->size);
				pcmChunk->fragment->nextFragment = NULL;
				pcmChunk->fragment->size = decodedWireChunk->size;

				ret = 0;
			}
		}
		else {
			pcm_chunk_fragment_t *next = NULL;
			size_t s;

			tmpSize = decodedWireChunk->size;
			//heap_caps_aligned_alloc(sizeof(uint32_t), decodedWireChunk->size, MALLOC_CAP_32BIT);
			pcmChunk->fragment->payload = (char *)heap_caps_malloc(largestFreeBlock, MALLOC_CAP_32BIT);
			if (pcmChunk->fragment->payload == NULL) {
				ESP_LOGE(
						TAG,
						"Failed to allocate memory for pcm chunk fragmented payload");

				free_pcm_chunk(pcmChunk);

				ret = -2;
			}
			else {
				next = pcmChunk->fragment;
				s = largestFreeBlock;

				// loop until we have all data stored to a fragment
				do {
					// copy the whole payload to our fragment
					memcpy(next->payload, decodedWireChunk->payload, s);
					next->size = s;
					tmpSize -= s;
					decodedWireChunk->payload += s;

					if (tmpSize > 0) {
						next->nextFragment = (pcm_chunk_fragment_t *)calloc(1, sizeof(pcm_chunk_fragment_t));
						if (next->nextFragment == NULL) {
							ESP_LOGE(
									TAG,
									"Failed to allocate memory for next pcm chunk fragment %d %d", heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

							free_pcm_chunk(pcmChunk);

							ret = -3;

							break;
						}
						else {
							largestFreeBlock = heap_caps_get_largest_free_block(MALLOC_CAP_32BIT);
							if (largestFreeBlock <= tmpSize) {
								s = largestFreeBlock;
								  // TODO: DMA buffer needs to be changed also to one chunk size if sample rate changes
							}
							else {
								s = tmpSize;
							}

							next->nextFragment->payload = (char *)heap_caps_malloc(s, MALLOC_CAP_32BIT);
							if (next->nextFragment->payload == NULL) {
								ESP_LOGE(
										TAG,
										"Failed to allocate memory for pcm chunk next fragmented payload");

								free_pcm_chunk(pcmChunk);

								ret = -3;

								break;
							}
							else {
								next = next->nextFragment;
							}
						}
					}
				} while(tmpSize);
			}
		}

		ret = 0;
	}
	else {
		// we got valid memory for pcm_chunk_message_t
		// no 32 bit aligned memory available, try to allocate 8 bit aligned memory for payload
		// check available memory first so we can decide if we need to fragment the data
		freeMem = heap_caps_get_free_size(MALLOC_CAP_8BIT);
		if (freeMem >= decodedWireChunk->size) {
			largestFreeBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
//			ESP_LOGI(
//					TAG,
//					"8b f %d b %d", freeMem, largestFreeBlock);
			if (largestFreeBlock >= decodedWireChunk->size) {
				pcmChunk->fragment->payload = (char *)heap_caps_malloc(decodedWireChunk->size, MALLOC_CAP_8BIT);
				if (pcmChunk->fragment->payload == NULL) {
					ESP_LOGE(
							TAG,
							"Failed to allocate memory for pcm chunk fragment payload");

					free_pcm_chunk(pcmChunk);

					ret = -2;
				}
				else {
					// copy the whole payload to our fragment
					memcpy(pcmChunk->fragment->payload, decodedWireChunk->payload, decodedWireChunk->size);
					pcmChunk->fragment->nextFragment = NULL;
					pcmChunk->fragment->size = decodedWireChunk->size;

					ret = 0;
				}
			}
			else {
				pcm_chunk_fragment_t *next = NULL;
				size_t s;

				tmpSize = decodedWireChunk->size;
				//heap_caps_aligned_alloc(sizeof(uint32_t), decodedWireChunk->size, MALLOC_CAP_32BIT);
				pcmChunk->fragment->payload = (char *)heap_caps_malloc(largestFreeBlock, MALLOC_CAP_8BIT);
				if (pcmChunk->fragment->payload == NULL) {
					ESP_LOGE(
							TAG,
							"Failed to allocate memory for pcm chunk fragmented payload");

					free_pcm_chunk(pcmChunk);

					ret = -2;
				}
				else {
					next = pcmChunk->fragment;
					s = largestFreeBlock;

					// loop until we have all data stored to a fragment
					do {
						// copy the whole payload to our fragment
						memcpy(next->payload, decodedWireChunk->payload, s);
						next->size = s;
						tmpSize -= s;
						decodedWireChunk->payload += s;

						if (tmpSize > 0) {
							next->nextFragment = (pcm_chunk_fragment_t *)calloc(1, sizeof(pcm_chunk_fragment_t));
							if (next->nextFragment == NULL) {
								ESP_LOGE(
										TAG,
										"Failed to allocate memory for next pcm chunk fragment %d %d", heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

								free_pcm_chunk(pcmChunk);

								ret = -3;

								break;
							}
							else {
								largestFreeBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
								if (largestFreeBlock <= tmpSize) {
									s = largestFreeBlock;
								}
								else {
									s = tmpSize;
								}

								next->nextFragment->payload = (char *)heap_caps_malloc(s, MALLOC_CAP_8BIT);
								if (next->nextFragment->payload == NULL) {
									ESP_LOGE(
											TAG,
											"Failed to allocate memory for pcm chunk next fragmented payload");

									free_pcm_chunk(pcmChunk);

									ret = -3;

									break;
								}
								else {
									next = next->nextFragment;
								}
							}
						}
					} while(tmpSize);
				}
			}

			ret = 0;
		}
	}
#endif

	if (ret == 0) {
		if (xQueueSendToBack(pcmChunkQueueHandle,
						   &pcmChunk,
						   pdMS_TO_TICKS(1000)) != pdTRUE)
		{
			ESP_LOGW(
				TAG,
				"send: pcmChunkQueue full, messages waiting %d",
				uxQueueMessagesWaiting(pcmChunkQueueHandle));

			free_pcm_chunk(pcmChunk);
		}
	}
	else {
		ESP_LOGE(TAG, "couldn't get memory to insert fragmented chunk, %d", freeMem);
	}

	return ret;
}

/**
 *
 */
static void snapcast_sync_task(void *pvParameters) {
  pcm_chunk_message_t *chnk = NULL;
  int64_t serverNow = 0;
  int64_t age;
  BaseType_t ret;
  int64_t chunkDuration_us;// = WIRE_CHUNK_DURATION_MS * 1000;
  char *p_payload = NULL;
  size_t size = 0;
  uint32_t notifiedValue;
  snapcastSetting_t snapcastSetting;
  uint64_t timer_val;
  const int32_t alarmValSub = 0;
  int initialSync = 0;
  int64_t avg = 0;
  int dir = 0;
  int64_t buffer_us_local = 0;
  pcm_chunk_fragment_t *fragment = NULL;
  size_t written;
  bool gotSnapserverConfig = false;

  ESP_LOGI(TAG, "started sync task");

  snapcastSettingQueueHandle = xQueueCreate(1, sizeof(snapcastSetting_t));

  initialSync = 0;

  shortMedianFilter.numNodes = SHORT_BUFFER_LEN;
  shortMedianFilter.medianBuffer = shortMedianBuffer;
  if (MEDIANFILTER_Init(&shortMedianFilter)) {
    ESP_LOGE(TAG, "snapcast_sync_task: couldn't init shortMedianFilter. STOP");

    return;
  }

  while (1) {
	  // check if we got changed setting available, if so we need to reinitialize
	  ret = xQueueReceive(snapcastSettingQueueHandle, &snapcastSetting, 0);
	  if (ret == pdTRUE) {
		  buffer_us_local = (int64_t)(snapcastSetting.buffer_ms) * 1000LL;

		  chunkDuration_us = (int64_t)(snapcastSetting.chunkDuration_ms) * 1000LL;
		  chunkInBytes = (snapcastSetting.chunkDuration_ms * snapcastSetting.sampleRate * snapcastSetting.channels * (snapcastSetting.bits / 8)) / 1000;

		  if ((snapcastSetting.sampleRate > 0) && (snapcastSetting.bits) > 0 && (snapcastSetting.channels > 0))  {
			  i2s_custom_stop(I2S_NUM_0);

			  ret = player_setup_i2s(I2S_NUM_0, &currentSnapcastSetting);
			  if (ret < 0) {
			    ESP_LOGE(TAG, "player_setup_i2s failed: %d", ret);

			    return;
			  }

			  currentDir = 1;  // force adjust_apll to set correct playback speed
			  adjust_apll(0);

			  i2s_custom_set_clk(I2S_NUM_0, snapcastSetting.sampleRate, snapcastSetting.bits, snapcastSetting.channels);
			  initialSync = 0;
		  }

		  ESP_LOGI(TAG, "snapserver config changed, buffer %dms, chunk %dms, sample rate %d, ch %d, bits %d", snapcastSetting.buffer_ms, snapcastSetting.chunkDuration_ms, snapcastSetting.sampleRate, snapcastSetting.channels, snapcastSetting.bits);

		  gotSnapserverConfig = true;
	  }
	  else if (gotSnapserverConfig == false) {
		  vTaskDelay(pdMS_TO_TICKS(10));

		  continue;
	  }

    if (chnk == NULL) {
      ret = xQueueReceive(pcmChunkQueueHandle, &chnk, pdMS_TO_TICKS(2000));
      if (ret != pdFAIL) {
        //				ESP_LOGW(TAG, "got pcm chunk");
      }
    } else {
      //			ESP_LOGW(TAG, "already retrieved chunk needs
      // service");
      ret = pdPASS;
    }

    if (ret != pdFAIL) {
      if (server_now(&serverNow) >= 0) {
        age = serverNow -
              ((int64_t)chnk->timestamp.sec * 1000000LL +
               (int64_t)chnk->timestamp.usec) -
              (int64_t)buffer_us_local +
              (int64_t)clientDacLatency;
      } else {
        // ESP_LOGW(TAG, "couldn't get server now");

        if (chnk != NULL) {
        	free_pcm_chunk(chnk);
        	chnk = NULL;
        }

        vTaskDelay(pdMS_TO_TICKS(1));

        continue;
      }

	  // wait for early time syncs to be ready
	  int tmp = latency_buffer_full();
	  if ( tmp <= 0 ) {
		if (tmp < 0) {
		  vTaskDelay(1);

		  continue;
		}

		if (age >= 0) {
		  if (chnk != NULL) {
			  free_pcm_chunk(chnk);
			  chnk = NULL;
		  }
		}

		// ESP_LOGW(TAG, "diff buffer not full");

		vTaskDelay( pdMS_TO_TICKS(10) );

		continue;
      }

      if (age < 0) {  // get initial sync using hardware timer
        if (initialSync == 0) {
          tg0_timer1_start(-age -
						 alarmValSub);  // alarm a little earlier to account
										// for context switch duration from
										// freeRTOS, timer with 1µs ticks

//          tg0_timer1_start((-age * 10) - alarmValSub));	// alarm a
															// little earlier to account for context switch duration from
															// freeRTOS, timer with 100ns ticks

          i2s_custom_stop(I2S_NUM_0);

          if (MEDIANFILTER_Init(&shortMedianFilter)) {
            ESP_LOGE(
                TAG,
                "snapcast_sync_task: couldn't init shortMedianFilter. STOP");

            return;
          }

          adjust_apll(0);  // reset to normal playback speed

          fragment = chnk->fragment;
		  p_payload = fragment->payload;
		  size = fragment->size;

          i2s_custom_init_dma_tx_queues(I2S_NUM_0, (uint8_t *)p_payload, size, &written);
          size -= written;
          p_payload += written;

//          ESP_LOGE(TAG, "wrote %d", written);

    	  if (size == 0) {
    		  if (fragment->nextFragment != NULL) {
    			  fragment = fragment->nextFragment;
    			  p_payload = fragment->payload;
    			  size = fragment->size;
    		  }
    		  else {
    			  free_pcm_chunk(chnk);
    			  chnk = NULL;
    		  }
    	  }

          // Wait to be notified of a timer interrupt.
          xTaskNotifyWait(pdFALSE,         // Don't clear bits on entry.
                          pdFALSE,         // Don't clear bits on exit.
                          &notifiedValue,  // Stores the notified value.
                          portMAX_DELAY);
          // or use simple task delay for this
          // vTaskDelay( pdMS_TO_TICKS(-age / 1000) );

          i2s_custom_start(I2S_NUM_0);

          // get timer value so we can get the real age
          timer_get_counter_value(TIMER_GROUP_0, TIMER_1, &timer_val);
          timer_pause(TIMER_GROUP_0, TIMER_1);

          // get actual age after alarm
//           age = ((int64_t)timer_val - (-age) * 10) / 10;	// timer with 100ns ticks
          age = (int64_t)timer_val - (-age);  // timer with 1µs ticks

          // check if we need to write remaining data
          if (size != 0) {
			  do {
				  written = 0;
				  if (i2s_custom_write(I2S_NUM_0, p_payload, (size_t)size, &written,
								portMAX_DELAY) != ESP_OK) {
					ESP_LOGE(TAG, "i2s_playback_task: I2S write error");
				  }
				  if (written < size) {
					  ESP_LOGE(TAG, "i2s_playback_task: I2S didn't write all data");
				  }
				  size -= written;
				  p_payload += written;

				  if (size == 0) {
					  if (fragment->nextFragment != NULL) {
						  fragment = fragment->nextFragment;
						  p_payload = fragment->payload;
						  size = fragment->size;
					  }
					  else {
						  free_pcm_chunk(chnk);
						  chnk = NULL;

						  break;
					  }
				  }
			  } while(1);
          }

          initialSync = 1;

          ESP_LOGI(TAG, "initial sync %lldus", age);

          continue;
        }
      } else if ((age > 0) && (initialSync == 0)) {
        if (chnk != NULL) {
        	free_pcm_chunk(chnk);
        	chnk = NULL;
        }

        int64_t t;
        get_diff_to_server(&t);
        ESP_LOGW(TAG, "RESYNCING HARD 1 %lldus, %lldus", age, t);

        dir = 0;

        initialSync = 0;

        continue;
      }

      if (initialSync == 1) {
        const uint8_t enableControlLoop = 1;
        const int64_t age_expect = -chunkDuration_us * 1;	// this value is highly coupled with I2S DMA buffer size.
        													// DMA buffer has a size of 1 chunk (e.g. 20ms) so next chunk
        													// we get from queue will be -20ms
        const int64_t maxOffset = 50;              //µs, softsync 1
        const int64_t hardResyncThreshold = 3000;   //µs, hard sync

        avg = MEDIANFILTER_Insert(&shortMedianFilter, age + (-age_expect));
        if (MEDIANFILTER_isFull(&shortMedianFilter) == 0) {
          avg = age + (-age_expect);
        } else {
          // resync hard if we are off too far
          if ((avg < -hardResyncThreshold) || (avg > hardResyncThreshold) ||
              (initialSync == 0)) {
            if (chnk != NULL) {
            	free_pcm_chunk(chnk);
            	chnk = NULL;
            }

            int64_t t;
            get_diff_to_server(&t);
            ESP_LOGW(TAG, "RESYNCING HARD 2 %lldus, %lldus, %lldus", age, avg,
                     t);

            initialSync = 0;

            continue;
          }
        }

        if (enableControlLoop == 1) {
          if (avg < -maxOffset) {  // we are early
            dir = -1;
          } else if ((avg >= -maxOffset) && (avg <= maxOffset)) {
            dir = 0;
          } else if (avg > maxOffset) {  // we are late
            dir = 1;
          }

          adjust_apll(dir);
        }

        fragment = chnk->fragment;
		p_payload = fragment->payload;
		size = fragment->size;

		do {
		  written = 0;
		  if (i2s_custom_write(I2S_NUM_0, p_payload, (size_t)size, &written,
						portMAX_DELAY) != ESP_OK) {
			ESP_LOGE(TAG, "i2s_playback_task: I2S write error");
		  }
		  if (written < size) {
			  ESP_LOGE(TAG, "i2s_playback_task: I2S didn't write all data");
		  }
		  size -= written;
		  p_payload += written;

		  if (size == 0) {
			  if (fragment->nextFragment != NULL) {
				  fragment = fragment->nextFragment;
				  p_payload = fragment->payload;
				  size = fragment->size;

				  ESP_LOGI(TAG, "i2s_playback_task: fragmented");
			  }
			  else {
				  free_pcm_chunk(chnk);
				  chnk = NULL;

				  break;
			  }
		  }
		} while(1);
      }

      int64_t t;
      get_diff_to_server(&t);
      ESP_LOGI(TAG, "%d: %lldus, %lldus %lldus", dir, age, avg, t);
    } else {
      int64_t t;
      get_diff_to_server(&t);
      ESP_LOGE(
          TAG,
          "Couldn't get PCM chunk, recv: messages waiting %d, latency %lldus",
          uxQueueMessagesWaiting(pcmChunkQueueHandle), t);

      dir = 0;

      initialSync = 0;
    }
  }
}