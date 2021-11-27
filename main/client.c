/* HTTP GET Example using plain POSIX sockets

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#define LED_PIN 27

static void led_init(void)
{
  // Configure the pin for the LED
  gpio_config_t io_conf;
  io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pin_bit_mask = ((uint64_t)1 << LED_PIN);
  io_conf.pull_down_en = 0;
  io_conf.pull_up_en = 0;
  gpio_config(&io_conf);
}

static void led_set(int level)
{
  gpio_set_level(LED_PIN, level);
}

/* The examples use simple WiFi configuration that you can set via
   'make menuconfig'.

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
 */
#define EXAMPLE_WIFI_SSID CONFIG_WIFI_SSID
#define EXAMPLE_WIFI_PASS CONFIG_WIFI_PASSWORD

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;

#define QUOTE_(str) #str
#define QUOTE(str) QUOTE_(str)

/* Constants that aren't configurable in menuconfig */
#define WEB_SERVER CONFIG_WEB_SERVER
#define WEB_PORT CONFIG_WEB_PORT
#define WEB_URL CONFIG_WEB_URL

static const char *TAG = "lightswitch";

static const char *REQUEST = "GET " WEB_URL " HTTP/1.1\r\n"
"Host: " WEB_SERVER ":" QUOTE(WEB_PORT) "\r\n"
"Upgrade: websocket\r\n"
"Connection: Upgrade\r\n"
"Sec-WebSocket-Key: HOUd4Hy32TFDnzR541Pw/Q==\r\n"
"Origin: http://" WEB_SERVER ":" QUOTE(WEB_PORT) "\r\n"
"Sec-WebSocket-Protocol: lighting\r\n"
"Sec-WebSocket-Version: 13\r\n"
"User-Agent: esp-idf/1.0 esp32 lightswitch\r\n"
"\r\n";

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
        switch(event->event_id) {
                case SYSTEM_EVENT_STA_START:
                        esp_wifi_connect();
                        break;
                case SYSTEM_EVENT_STA_GOT_IP:
                        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
                        break;
                case SYSTEM_EVENT_STA_DISCONNECTED:
                        /* This is a workaround as ESP32 WiFi libs don't currently
                           auto-reassociate. */
                        esp_wifi_connect();
                        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
                        break;
                default:
                        break;
        }
        return ESP_OK;
}

static void initialise_wifi(void)
{
        tcpip_adapter_init();
        wifi_event_group = xEventGroupCreate();
        ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
        ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
        wifi_config_t wifi_config = {
                .sta = {
                        .ssid = EXAMPLE_WIFI_SSID,
                        .password = EXAMPLE_WIFI_PASS,
                },
        };
        ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
        ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
        ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
        ESP_ERROR_CHECK( esp_wifi_start() );
}

/* Hexdump helper code */
char hex(unsigned char h)
{
        char r[] = "0123456789abcdef";
        return r[h];
}

void hexdump(const unsigned char* const buffer, int len)
{
        unsigned char dump[len * 3 + 2];
        unsigned char* ptr = &dump[0];

        *ptr++ = '[';

        for (int i = 0; i < len; ++i)
        {
                *ptr++ = hex((buffer[i] >> 4) & 0x0f);
                *ptr++ = hex(buffer[i] & 0x0f);
                *ptr++ = ' ';
        }

        *ptr = ']';

        *ptr++ = '\0';

        ESP_LOGI(TAG, "Hex(%d): %s", len, dump);
}

/* Websocket structures */

/* Frame layout

   0                   1                   2                   3
   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-------+-+-------------+-------------------------------+
   |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
   |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
   |N|V|V|V|       |S|             |   (if payload len==126/127)   |
   | |1|2|3|       |K|             |                               |
   +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
   |     Extended payload length continued, if payload len == 127  |
   + - - - - - - - - - - - - - - - +-------------------------------+
   |                               |Masking-key, if MASK set to 1  |
   +-------------------------------+-------------------------------+
   | Masking-key (continued)       |          Payload Data         |
   +-------------------------------- - - - - - - - - - - - - - - - +
   :                     Payload Data continued ...                :
   + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
   |                     Payload Data continued ...                |
   +---------------------------------------------------------------+

 */

typedef enum
{
        OC_CONT = 0x00,
        OC_TEXT = 0x01,
        OC_BINARY = 0x02,
        // 3-7 reserved
        OC_CLOSE = 0x08,
        OC_PING = 0x09,
        OC_PONG = 0x0a,
        // b-f reserved
} ws_opcode;

// Low level frame building
unsigned char ws_recv_buffer[1024 * 8];
unsigned char ws_send_buffer[1024 * 8];

unsigned char* ws_send_ptr = NULL;

#define DR_REG_RNG_BASE 0x3ff75144
uint32_t ws_mask = 0x00000000;

void ws_write_header(char fin, char opcode, char mask, int payload_len)
{
        int payload_len_ext = 0;

        // Randomise the mask
        ws_mask = READ_PERI_REG(DR_REG_RNG_BASE);

        // TODO: Support 64 bit payload length
        if (payload_len >= 126)
        {
                payload_len_ext = payload_len - 126;
                payload_len = 126;
        }

        ws_send_buffer[0] = (fin ? 0x80 : 0x00) | (opcode & 0x0f);
        ws_send_buffer[1] = (mask ? 0x80 : 0x00) | (payload_len & 0x7f);

        ws_send_ptr = &ws_send_buffer[2];

        if (payload_len_ext > 0)
        {
                ws_send_buffer[2] = (payload_len_ext >> 8) & 0xff;
                ws_send_buffer[3] = (payload_len_ext) & 0xff;

                ws_send_ptr += 2;
        }
}

void ws_add_masking_key(void)
{
        memcpy(ws_send_ptr, &ws_mask, 4);
        ws_send_ptr += 4;
}

void ws_add_masked_data(const char* const data, int length)
{
        for (int i = 0; i < length; ++i)
        {
                (*ws_send_ptr++) = data[i] ^ ((char*)&ws_mask)[i % 4];
        }
}

int ws_get_send_length(void)
{
        return (int)(ws_send_ptr - ws_send_buffer);
}

int ws_send_frame(int s)
{
        hexdump(ws_send_buffer, ws_get_send_length());
        return write(s, ws_send_buffer, ws_get_send_length());
}

// High level frame building
int ws_send_ping(int s)
{
        ws_write_header(1, OC_PING, 1, 0);
        ws_add_masking_key();
        return ws_send_frame(s);
}

int ws_send_text(int s, const char* const buffer)
{
        ws_write_header(1, OC_BINARY, 1, strlen(buffer));
        ws_add_masking_key();
        ws_add_masked_data(buffer, strlen(buffer));
        ws_send_buffer[ws_get_send_length()] = 0;
        return ws_send_frame(s);
}

#define SWITCH_PIN 23

#define SWITCH_FLAG_ALL_ON   1
#define SWITCH_FLAG_ALL_OFF  2

typedef struct {
  const char* const name; // Name of this switch
  uint8_t pin;            // Pin the switch is attached to
  int pattern_on;         // Pattern to send to turn lights on
  int pattern_off;        // Pattern to send to turn lights off
  int flags;              // Flags bitfield
} switch_t;

typedef struct {
  uint8_t level;          // State of the switch
  uint8_t state;          // The last known state of the lights
} state_t;

#define NUM_SWITCHES (sizeof(switches) / sizeof(switch_t))

switch_t switches[] = {
  {
    .name = "Dusty On",
    .pin = 33,
    .pattern_on = 186,
    .pattern_off = 186,
    .flags = 0,
  },
  {
    .name = "Workshop All On",
    .pin = 25,
    .pattern_on = 139,
    .pattern_off = 139,
    .flags = SWITCH_FLAG_ALL_ON,
  },
  {
    .name = "Workshop All Off",
    .pin = 26,
    .pattern_on = 142,
    .pattern_off = 142,
    .flags = SWITCH_FLAG_ALL_OFF,
  },
  {
    .name = "Woodwork On",
    .pin = 23,
    .pattern_on = 204,
    .pattern_off = 204,
    .flags = 0,
  },
  {
    .name = "Metalwork On",
    .pin = 22,
    .pattern_on = 192,
    .pattern_off = 192,
    .flags = 0,
  },
  {
    .name = "Walk Way Far On",
    .pin = 21,
    .pattern_on = 174,
    .pattern_off = 174,
    .flags = 0,
  },
  {
    .name = "Walk Way Near On",
    .pin = 19,
    .pattern_on = 168,
    .pattern_off = 168,
    .flags = 0,
  },
  {
    .name = "Laser On",
    .pin = 18,
    .pattern_on = 180,
    .pattern_off = 180,
    .flags = 0,
  }
};

state_t states[NUM_SWITCHES] = { 0 };

void lightswitch_set_all_states(int state)
{
  for (int state_idx = 0; state_idx < NUM_SWITCHES; ++state_idx)
  {
    states[state_idx].state = state;
  }
}

static void lightswitch_task(void *pvParameters)
{
  const struct addrinfo hints = {
    .ai_family = AF_INET,
    .ai_socktype = SOCK_STREAM,
  };
  struct addrinfo *res;
  struct in_addr *addr;
  int s, r;
  char recv_buf[64];

  char pattern_request_buffer[256];

  ESP_LOGI(TAG, "Initialising switches...");

  // Configure each GPIO for input
  {
    for (int switch_idx = 0; switch_idx < NUM_SWITCHES; ++switch_idx)
    {
      const int pin = switches[switch_idx].pin;

      gpio_config_t io_conf;
      io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
      io_conf.mode = GPIO_MODE_INPUT;
      io_conf.pin_bit_mask = ((uint64_t)1 << pin);
      io_conf.pull_down_en = 0;
      io_conf.pull_up_en = 1;
      gpio_config(&io_conf);

      const int level = gpio_get_level(pin);
      states[switch_idx].level = level;

      ESP_LOGI(TAG, "Initial status of switch %s: %d", switches[switch_idx].name, level);
    }
  }

  // Poll and update loop
  do {
    // Wait until any switch changes state
    int state_change = 0;

    // Pattern to send
    int pattern_id = 0;

    // LED off while waiting for input
    led_set(0);

    do {
      // Scan the switches for a low going state change
      for (int switch_idx = 0; switch_idx < NUM_SWITCHES; ++switch_idx)
      {
        const int pin = switches[switch_idx].pin;
        const int new_level = gpio_get_level(pin);
        if (new_level != states[switch_idx].level)
        {
          states[switch_idx].level = new_level;

          ESP_LOGI(TAG, "Switch %s %s.", switches[switch_idx].name, new_level ? "released" : "pressed");

          // Switch pressed
          if (new_level == 0)
          {
            // Stop scanning and send the pattern for this switch
            state_change = 1;

            if (states[switch_idx].state)
            {
              // Light is on, needs to be off
              pattern_id = switches[switch_idx].pattern_off;

              // Update the last known state of the lights
              states[switch_idx].state = 0;
            }
            else
            {
              // Light is off, needs to be on
              pattern_id = switches[switch_idx].pattern_on;

              // Update the last known state of the lights
              states[switch_idx].state = 1;
            }

            // Update the last known state of all lights if necessary
            if (switches[switch_idx].flags & SWITCH_FLAG_ALL_ON)
            {
              lightswitch_set_all_states(1);
              ESP_LOGI(TAG, "Set all states to ON.");
            }
            else if (switches[switch_idx].flags & SWITCH_FLAG_ALL_OFF)
            {
              lightswitch_set_all_states(0);
              ESP_LOGI(TAG, "Set all states to OFF.");
            }

            break;
          }
        }
      }

      vTaskDelay(100 / portTICK_PERIOD_MS);
      esp_task_wdt_feed();
    } while (!state_change);

    // LED on while sending the request
    led_set(1);

    // Print out the pattern we're sending
    ESP_LOGI(TAG, "Sending pattern id %d...", pattern_id);

    /* Wait for the callback to set the CONNECTED_BIT in the
       event group.
       */
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
        false, true, portMAX_DELAY);
    ESP_LOGI(TAG, "Connected to AP");

    int err = getaddrinfo(WEB_SERVER, QUOTE(WEB_PORT), &hints, &res);

    if(err != 0 || res == NULL) {
      ESP_LOGE(TAG, "DNS lookup failed err=%d res=%p", err, res);
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      continue;
    }

    /* Code to print the resolved IP.
      Note: inet_ntoa is non-reentrant, look at ipaddr_ntoa_r for "real" code
    */
    addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
    ESP_LOGI(TAG, "DNS lookup succeeded. IP=%s", inet_ntoa(*addr));

    s = socket(res->ai_family, res->ai_socktype, 0);
    if(s < 0) {
      ESP_LOGE(TAG, "... Failed to allocate socket.");
      freeaddrinfo(res);
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      continue;
    }
    ESP_LOGI(TAG, "... allocated socket\r\n");

    if(connect(s, res->ai_addr, res->ai_addrlen) != 0) {
      ESP_LOGE(TAG, "... socket connect failed errno=%d", errno);
      close(s);
      freeaddrinfo(res);
      vTaskDelay(4000 / portTICK_PERIOD_MS);
      continue;
    }

    ESP_LOGI(TAG, "... connected");
    freeaddrinfo(res);

    if (write(s, REQUEST, strlen(REQUEST)) < 0) {
      ESP_LOGE(TAG, "... socket send failed");
      close(s);
      vTaskDelay(4000 / portTICK_PERIOD_MS);
      continue;
    }
    ESP_LOGI(TAG, "... socket send success");

    /* Read HTTP response */
    int nlc = 0;
    do {
      bzero(recv_buf, sizeof(recv_buf));
      r = read(s, recv_buf, sizeof(recv_buf)-1);
      for(int i = 0; i < r; i++) {
        putchar(recv_buf[i]);
        //ESP_LOGI(TAG, "... nlc: %d c: %x", nlc, send_buf[i]);
        if (nlc % 2 == 0)
        {
          if (recv_buf[i] == '\r')
          {
            ++nlc;
          }
          else
          {
            nlc = 0;
          }
        }
        else if (nlc % 2 == 1)
        {
          if (recv_buf[i] == '\n')
          {
            ++nlc;
          }
          else
          {
            nlc = 0;
          }
        }
        if (nlc == 3) { r = 0; }
      }
    } while(r > 0);
    ESP_LOGI(TAG, "...read a response");

    /*
       if (ws_send_text(s, "{\"eventType\": \"ConnectRequest\", \"token\": \"9p5cNFsViBtysW4RBtPwemH0ZuLcZUl031i4dP3r\"}") < 0)
       {
       ESP_LOGE(TAG, "... send connect failed");
       close(s);
       vTaskDelay(4000 / portTICK_PERIOD_MS);
       continue;
       }
       ESP_LOGI(TAG, "... send connect success");
       */

    snprintf(pattern_request_buffer, 256, "{\"eventType\": \"PatternRequest\", \"patternId\": %d}", pattern_id);
    if (ws_send_text(s, pattern_request_buffer) < 0)
    {
      ESP_LOGE(TAG, "... send request failed");
      close(s);
      vTaskDelay(4000 / portTICK_PERIOD_MS);
      continue;
    }
    ESP_LOGI(TAG, "... send request success");

    /*
       do {
       bzero(recv_buf, sizeof(recv_buf));
       r = read(s, recv_buf, sizeof(recv_buf)-1);
       for(int i = 0; i < r; i++) {
       putchar(recv_buf[i]);
    //ESP_LOGI(TAG, "[%x]", recv_buf[i]);
    }
    } while(r > 0);
    ESP_LOGI(TAG, "...read text response");
    */

    ESP_LOGI(TAG, "... done reading from socket. Last read return=%d errno=%d\r\n", r, errno);
    close(s);
  } while (1);
}

void app_main()
{
  ESP_ERROR_CHECK( nvs_flash_init() );

  // LED on once we're initialising
  led_init();
  led_set(1);

  initialise_wifi();
  xTaskCreate(&lightswitch_task, "lightswitch_task", 4096, NULL, 5, NULL);

  // LED off once we're initialised
  led_set(0);
}

