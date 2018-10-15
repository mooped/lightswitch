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

typedef struct
{
  uint8_t fin;
  ws_opcode opcode;

  uint8_t mask_bit;
  uint32_t payload_len;

  uint32_t mask;
} ws_header_t;

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

int ws_recv_header(int s, ws_header_t* pHeader)
{
  int r = 0;
  unsigned char* recv_ptr = ws_recv_buffer;

  if (!pHeader) { return -1; }

  // Clear enough of the receive buffer for the header
  bzero(recv_ptr, sizeof(ws_recv_buffer));

  // Read the first two header bytes
  r = read(s, recv_ptr, 2);
  if (r != 2) { return -1; }

  // Fin flag and opcode from the first byte
  pHeader->fin = recv_ptr[0] & 0x80;
  pHeader->opcode = recv_ptr[0] & 0x0f;

  // Mask bit and 7 bit payload length field from the second byte
  pHeader->mask_bit = (recv_ptr[1] & 0x80) ? 1 : 0;
  pHeader->payload_len = recv_ptr[1] & 0x7f;

  recv_ptr += 2;

  // Read additional message length if necessary
  if (pHeader->payload_len == 126)
  {
    r = read(s, recv_ptr, 2);
    if (r != 2) { return -1; }
    pHeader->payload_len = (uint32_t)*((uint16_t*)recv_ptr);
    recv_ptr += 2;
  }
  else if (pHeader->payload_len == 127)
  {
    // We can only cope with 32 bit packet length - those 4gb+ packets will get truncated...
    r = read(s, recv_ptr, 8);
    if (r != 8) { return -1; }
    pHeader->payload_len = *((uint32_t*)recv_ptr);
    recv_ptr += 8;
  }

  // Read mask if necessary
  if (pHeader->mask_bit)
  {
    r = read(s, recv_ptr, 4);
    if (r != 4) { return -1; }
    pHeader->mask = *((uint32_t*)recv_ptr);
    recv_ptr += 4;
  }

  return recv_ptr - ws_recv_buffer;
}

int ws_recv_data(int s, ws_header_t* pHeader, unsigned char* buffer)
{
  if (!pHeader) { return -1; }
  return read(s, buffer, pHeader->payload_len);
}

#define SWITCH_PIN 23

static void hauntspace_task(void *pvParameters)
{
        const struct addrinfo hints = {
                .ai_family = AF_INET,
                .ai_socktype = SOCK_STREAM,
        };
        struct addrinfo *res;
        struct in_addr *addr;
        int s, r;
        char recv_buf[64];

        // Configure GPIO for input
        /*
        gpio_config_t io_conf;
        io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1 << SWITCH_PIN);
        io_conf.pull_down_en = 0;
        io_conf.pull_up_en = 1;
        gpio_config(&io_conf);

        level = gpio_get_level(SWITCH_PIN);
        ESP_LOGI(TAG, "Initial switch status: %d", level);
        */

        // Connect and listen until disconnected
        do {
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
                  Note: inet_ntoa is non-reentrant, look at ipaddr_ntoa_r for "real" code */
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

                /* Send HTTP request with websocket upgrade header */
                if (write(s, REQUEST, strlen(REQUEST)) < 0) {
                        ESP_LOGE(TAG, "... header send failed");
                        close(s);
                        vTaskDelay(4000 / portTICK_PERIOD_MS);
                        continue;
                }
                ESP_LOGI(TAG, "... header send success");

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
                ESP_LOGI(TAG, "... read response header success");

                if (ws_send_text(s, "{\"eventType\": \"ConnectRequest\", \"token\": \"9p5cNFsViBtysW4RBtPwemH0ZuLcZUl031i4dP3r\"}") < 0)
                {
                  ESP_LOGE(TAG, "... send connection request failed");
                  close(s);
                  vTaskDelay(4000 / portTICK_PERIOD_MS);
                  continue;
                }
                ESP_LOGI(TAG, "... send connection request success");

                ESP_LOGI(TAG, "waiting for packets ...");
                ws_header_t header;
                bzero(&header, sizeof(header));
                while (ws_recv_header(s, &header) != -1)
                {
                  ESP_LOGI(TAG, "... got header");
                  int recv_len = ws_recv_data(s, &header, ws_recv_buffer);
                  ws_recv_buffer[recv_len] = '\0';
                  ESP_LOGI(TAG, "... got data - len: %d payload: %s\r\n", recv_len, ws_recv_buffer);
                }

                ESP_LOGI(TAG, "... socket closed. Last read return=%d errno=%d\r\n", r, errno);
                close(s);
        } while (1);
}

void app_main()
{
        ESP_ERROR_CHECK( nvs_flash_init() );
        initialise_wifi();
        xTaskCreate(&hauntspace_task, "hauntspace_task", 4096, NULL, 5, NULL);
}

