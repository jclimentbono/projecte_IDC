#include <stdio.h>
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h" 
#include "dht.h"


// Ver que pin corresponde a que 
#define ADC_SENSORS_UNIT ADC_UNIT_1 
#define ADC_LUX ADC_CHANNEL_3 
#define ADC_HUMI ADC_CHANNEL_1 
#define DHT_GPIO_PIN GPIO_NUM_16

// Datos de los pins  Analog Digital Convertet
#define ATTEN ADC_ATTEN_DB_11
#define BITWIDTH ADC_BITWIDTH_12

//calibración para los sensor
#define MAX_ADC_VALUE 4095.0
#define HUM_SENSOR_WET 100
#define INTERVAL_DATA_COLLECTION (1000*60*58) // en ms

// Enum sirve para identificar de que sensor viene el dato
typedef enum {
    SENSOR_DHT11,
    SENSOR_LUX,
    SENSOR_SOIL
} sensor_type_t;

// Strcut que permite  guardar los datos enviados a la cola
typedef struct {
    sensor_type_t type;
    float value1; // Valores singulares y la temeperatura del aire con  DHT11
    float value2; // Humedad del aire con DHT11
} sensor_data_t;

// definición de la cola
static QueueHandle_t sensor_queue = NULL;

// definción de la unidad de adc
static adc_oneshot_unit_handle_t adc1_handle;

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

// ==========================================
// Lee la tempratura y humedad del aire para luego mandarla al 
// ==========================================
void getTemp(void *pvParameters){
    int16_t temp = 0;
    int16_t hum = 0;
    sensor_data_t packet;
    packet.type = SENSOR_DHT11;

    vTaskDelay(pdMS_TO_TICKS(3000));

    while(1){    
        esp_err_t res = dht_read_data(DHT_TYPE_DHT11, DHT_GPIO_PIN, &hum, &temp);
        
        if (res == ESP_OK) {
            packet.value1 = (float)temp / 10.0;
            packet.value2 = (float)hum / 10.0;
            
            // Send structure packet to the back of the queue (wait up to 100ms if queue is full)
            xQueueSend(sensor_queue, &packet, pdMS_TO_TICKS(100));
        }
        vTaskDelay(pdMS_TO_TICKS(INTERVAL_DATA_COLLECTION+1000));
    }
}

// Lee el dato del LDR 10 veces hace la media y lo manda a la cola
void getLux(void *pvParameters){
    int temp = 0, tmp;
    sensor_data_t packet;
    packet.type = SENSOR_LUX;
    packet.value2 = 0; // Unused

    while(1){    
        for(int i = 0; i < 10; i++){
            adc_oneshot_read(adc1_handle, ADC_LUX, &tmp);
            temp += tmp;
        }
        temp /= 10;
        
        packet.value1 = ((float)temp / MAX_ADC_VALUE) * 100.0;
        
        // Send packet to the queue
        xQueueSend(sensor_queue, &packet, pdMS_TO_TICKS(100));
        
        temp = 0;
        vTaskDelay(pdMS_TO_TICKS(INTERVAL_DATA_COLLECTION));
    }
}

// lee el dato del sensor de humedad de suelo 10 veces para hacer la media y lo manda a la cola
void getHumi(void *pvParameters){
    int humi = 0, tmp;
    sensor_data_t packet;
    packet.type = SENSOR_SOIL;
    packet.value2 = 0; // Unused

    while(1){
        for(int i = 0; i < 10; i++){
            adc_oneshot_read(adc1_handle, ADC_HUMI, &tmp);
            humi += tmp;
        }
        humi /= 10;
        
        packet.value1 = ((float)humi / MAX_ADC_VALUE) * 100.0;
        
       
        xQueueSend(sensor_queue, &packet, pdMS_TO_TICKS(100));
        
        humi = 0;
        vTaskDelay(pdMS_TO_TICKS(INTERVAL_DATA_COLLECTION+500));
    }
}

//
// FUNCIÓN A REMPLAZAR POR LA DE COMUNICACIÓN
//
void printDisplayTask(void *pvParameters){
    sensor_data_t received_packet;
    

    while(1){
        // portMAX_DELAY blocks this task indefinitely until an item arrives in the queue.
        // This consumes 0% CPU resources while sleeping/waiting!
        if (xQueueReceive(sensor_queue, &received_packet, portMAX_DELAY) == pdTRUE) {
            
            printf("---------------------------------------\n");
            switch(received_packet.type) {
                case SENSOR_DHT11:
                    printf("[RECEIVER] -> Datos de Aire:\n");
                    printf("   Temperatura: %.1f °C\n", received_packet.value1);
                    printf("   Humedad:     %.1f %%\n", received_packet.value2);
                    break;
                    
                case SENSOR_LUX:
                    printf("[RECEIVER] -> Datos de Lumetría:\n");
                    printf("   Intensidad de Luz: %.2f %%\n", received_packet.value1);
                    break;
                    
                case SENSOR_SOIL:
                    printf("[RECEIVER] -> Datos de Humedad de Suelo:\n");
                    printf("   Humedad Suelo:     %.2f %%\n", received_packet.value1);
                    break;
            }
            printf("---------------------------------------\n");
            fflush(stdout);
        }
    }
}

void app_main(void)
{
    init_adcs();
    
    // Create a queue capable of storing 10 "sensor_data_t" structures
    sensor_queue = xQueueCreate(10, sizeof(sensor_data_t));
    
    if (sensor_queue != NULL) {
        // tarea con mayor prioridad
        // a remplazar por la tarea de LoraWan o MQTT o HTTP
        xTaskCreate(printDisplayTask, "central_print", 4096, NULL, 5, NULL);
        
        // Crea las tareas con la menor prioridad que son los sensores 
        // recogiendo información 
        xTaskCreate(getTemp, "get_temp", 4096, NULL, 1, NULL);
        xTaskCreate(getLux,  "get_lux",  4096, NULL, 1, NULL);
        xTaskCreate(getHumi, "get_humi", 4096, NULL, 1, NULL);
    } else {
        printf("No se pudo crear la cola!\n");
        fflush(stdout);
    }
}