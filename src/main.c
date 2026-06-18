#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h" 
#include "esp_log.h"

static const char *TAG = "IDC";
// Config de lorawan
#define DEV_EUI  "70B3D57ED007815F"
#define JOIN_EUI "0000000000000000"
#define APP_KEY  "FCAB082D723133F2CD21AD101FC189F3"

//Config de pines
#define LORA_TX_PIN      18 // Conectado al RX del E5
#define LORA_RX_PIN      17 // Conectado al TX del E5
#define UART_NUM         UART_NUM_1

#define ADC_SENSORS_UNIT ADC_UNIT_1 
#define ADC_LUX ADC_CHANNEL_3 
#define ADC_HUMI ADC_CHANNEL_1 

#define ATTEN ADC_ATTEN_DB_12
#define BITWIDTH ADC_BITWIDTH_12

#define MAX_ADC_VALUE 4095.0
#define INTERVAL_DATA_COLLECTION 15000 // Tiempo de envío en ms

//Estructuras de cola
typedef enum {
    SENSOR_LUX,
    SENSOR_SOIL
} sensor_type_t;

typedef struct {
    sensor_type_t type;
    float value1; 
} sensor_data_t;

static QueueHandle_t display_queue = NULL;
static QueueHandle_t lora_queue = NULL;

static adc_oneshot_unit_handle_t adc1_handle;

//Inicialización de ADC
void init_adcs(){
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_SENSORS_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    adc_oneshot_new_unit(&init_config, &adc1_handle);

    adc_oneshot_chan_cfg_t channel_config = {
        .atten = ATTEN,
        .bitwidth = BITWIDTH,
    };
    
    adc_oneshot_config_channel(adc1_handle, ADC_HUMI, &channel_config);
    adc_oneshot_config_channel(adc1_handle, ADC_LUX, &channel_config);
}

//Sensores
void getLux(void *pvParameters){
    int temp = 0, tmp;
    sensor_data_t packet;
    packet.type = SENSOR_LUX;

    while(1){    
        for(int i = 0; i < 10; i++){
            adc_oneshot_read(adc1_handle, ADC_LUX, &tmp);
            temp += tmp;
        }
        temp /= 10;
        packet.value1 = ((float)temp / MAX_ADC_VALUE) * 100.0;
        
        xQueueSend(display_queue, &packet, pdMS_TO_TICKS(100));
        xQueueSend(lora_queue, &packet, pdMS_TO_TICKS(100));
        
        temp = 0;
        vTaskDelay(pdMS_TO_TICKS(INTERVAL_DATA_COLLECTION));
    }
}

void getHumi(void *pvParameters){
    int humi = 0, tmp;
    sensor_data_t packet;
    packet.type = SENSOR_SOIL;

    while(1){
        for(int i = 0; i < 10; i++){
            adc_oneshot_read(adc1_handle, ADC_HUMI, &tmp);
            humi += tmp;
        }
        humi /= 10;
        packet.value1 = ((float)humi / MAX_ADC_VALUE) * 100.0;
        
        xQueueSend(display_queue, &packet, pdMS_TO_TICKS(100));
        xQueueSend(lora_queue, &packet, pdMS_TO_TICKS(100));
        
        humi = 0;
        vTaskDelay(pdMS_TO_TICKS(INTERVAL_DATA_COLLECTION + 500)); // Pequeño desfase
    }
}

//Print por pantalla
void printDisplayTask(void *pvParameters){
    sensor_data_t received_packet;

    while(1){
        if (xQueueReceive(display_queue, &received_packet, portMAX_DELAY) == pdTRUE) {
            printf("---------------------------------------\n");
            if (received_packet.type == SENSOR_LUX) {
                printf("[PANTALLA] -> Intensidad de Luz: %.2f %%\n", received_packet.value1);
            } else if (received_packet.type == SENSOR_SOIL) {
                printf("[PANTALLA] -> Humedad Suelo:     %.2f %%\n", received_packet.value1);
            }
            printf("---------------------------------------\n");
            fflush(stdout);
        }
    }
}

//Lorawan
void loraWanTask(void *pvParameters){
    //Serial a 9600
    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_NUM, 1024, 0, 0, NULL, 0);
    uart_param_config(UART_NUM, &uart_config);
    uart_set_pin(UART_NUM, LORA_TX_PIN, LORA_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    ESP_LOGI(TAG, "Configurando módulo LoRa E5...");
    vTaskDelay(pdMS_TO_TICKS(2000));

    char cmd[128];

    // Comandos AT inicialización
    uart_write_bytes(UART_NUM, "AT+MODE=LWOTAA\r\n", 16);
    vTaskDelay(pdMS_TO_TICKS(1000));

    uart_write_bytes(UART_NUM, "AT+DR=EU868\r\n", 13);
    vTaskDelay(pdMS_TO_TICKS(500));

    snprintf(cmd, sizeof(cmd), "AT+ID=DevEui,\"%s\"\r\n", DEV_EUI);
    uart_write_bytes(UART_NUM, cmd, strlen(cmd));
    vTaskDelay(pdMS_TO_TICKS(500));

    snprintf(cmd, sizeof(cmd), "AT+ID=AppEui,\"%s\"\r\n", JOIN_EUI);
    uart_write_bytes(UART_NUM, cmd, strlen(cmd));
    vTaskDelay(pdMS_TO_TICKS(500));

    snprintf(cmd, sizeof(cmd), "AT+KEY=APPKEY,\"%s\"\r\n", APP_KEY);
    uart_write_bytes(UART_NUM, cmd, strlen(cmd));
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "Solicitando conexión a TTN (AT+JOIN)...");
    uart_write_bytes(UART_NUM, "AT+JOIN\r\n", 9);
    
    vTaskDelay(pdMS_TO_TICKS(12000));

    sensor_data_t received_packet;
    float last_lux = 0.0, last_soil = 0.0;
    int received_count = 0; 

    //Bucle principal
    while(1){
        if (xQueueReceive(lora_queue, &received_packet, portMAX_DELAY) == pdTRUE) {
            
            if (received_packet.type == SENSOR_LUX) {
                last_lux = received_packet.value1;
                received_count++;
            } else if (received_packet.type == SENSOR_SOIL) {
                last_soil = received_packet.value1;
                received_count++;
            }

            // Cuando tenemos el reporte de los 2 sensores, creamos y enviamos el payload
            if (received_count >= 2) {
                // Formato con comillas: AT+CMSGHEX="XXXX"
                snprintf(cmd, sizeof(cmd), "AT+CMSGHEX=\"%04X%04X\"\r\n", 
                         (int)(last_lux * 100) & 0xFFFF,
                         (int)(last_soil * 100) & 0xFFFF);
                         
                ESP_LOGI(TAG, "Enviando datos a TTN -> %04X%04X", 
                         (int)(last_lux * 100) & 0xFFFF, (int)(last_soil * 100) & 0xFFFF);
                         
                uart_write_bytes(UART_NUM, cmd, strlen(cmd));
                
                received_count = 0; 
            }
        }
    }
}


void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    fflush(stdout);

    init_adcs();
    
    display_queue = xQueueCreate(10, sizeof(sensor_data_t));
    lora_queue = xQueueCreate(10, sizeof(sensor_data_t));
    
    if (display_queue != NULL && lora_queue != NULL) {
        xTaskCreate(printDisplayTask, "central_print", 4096, NULL, 5, NULL);
        xTaskCreate(loraWanTask, "lorawan_task", 4096, NULL, 5, NULL);
        
        xTaskCreate(getLux,  "get_lux",  4096, NULL, 1, NULL);
        xTaskCreate(getHumi, "get_humi", 4096, NULL, 1, NULL);
    } else {
        printf("¡No se pudieron crear las colas!\n");
        fflush(stdout);
    }
}
