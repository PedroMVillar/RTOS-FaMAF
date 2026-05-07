# Joystick YXA197 — Driver FreeRTOS para STM32

Driver para el módulo joystick analógico YXA197 (ejes X/Y + botón SW + botones
opcionales), diseñado para FreeRTOS en STM32 usando únicamente la HAL de ST
(`vTaskDelay`/`vTaskDelayUntil`, sin `osDelay` ni capa CMSIS-RTOS). Desarrollado
y probado sobre la **Nucleo STM32F103RB** con STM32CubeIDE + STM32CubeMX.

---

## Archivos

```
Joystick_YXA197/
├── Inc/
│   └── joystick_yxa197.h     API pública, tipos y macros de configuración
├── Src/
│   └── joystick_yxa197.c     Implementación del driver
├── Examples/
│   └── joystick_example.c    Ejemplo comentado de integración
└── README.md                 Este archivo
```

---

## Requisitos de software

| Componente | Versión mínima |
|---|---|
| STM32CubeIDE | 1.13 o superior |
| STM32CubeMX (integrado) | 6.9 o superior |
| STM32CubeF1 HAL firmware | 1.8.5 |
| FreeRTOS | 10.x (incluido en CubeMX como middleware) |
| Compilador arm-none-eabi-gcc | Cualquiera incluido con CubeIDE |

---

## Requisitos de hardware

- Placa **STM32 Nucleo F103RB** (STM32F103RBT6, Cortex-M3, 72 MHz)
- Módulo **joystick YXA197** (o compatible: joystick analógico 5 pines con SW)
- El módulo opera a **3.3 V** → conectar VCC al pin 3.3V del Nucleo (no 5V)
- Cable/breadboard para conexiones

### Descripción de pines del módulo YXA197

| Pin módulo | Señal | Tipo |
|------------|-------|------|
| VCC | Alimentación | 3.3 V |
| GND | Referencia | GND |
| VRX | Eje X (potenciómetro horizontal) | Analógico 0–3.3V |
| VRY | Eje Y (potenciómetro vertical) | Analógico 0–3.3V |
| SW  | Botón al presionar el joystick | Digital (activo bajo) |

Algunos módulos traen botones adicionales (BTN1, BTN2, etc.) en pines extra.

### Conexiones sugeridas para Nucleo STM32F103RB

| Módulo YXA197 | Pin Nucleo | Función STM32 | Conector |
|---|---|---|---|
| VCC  | 3.3V | — | CN6-4 |
| GND  | GND  | — | CN6-6 |
| VRX  | PA0  | ADC1_IN0 | CN7-28 (Arduino A0) |
| VRY  | PA1  | ADC1_IN1 | CN7-30 (Arduino A1) |
| SW   | PB5  | GPIO Input | CN10-29 |
| BTN1 | PB4  | GPIO Input | CN10-27 (opcional) |
| BTN2 | PB3  | GPIO Input | CN10-25 (opcional) |

> **Nota:** Los pines PA0 y PA1 pueden soportar señales analógicas de 0 a 3.3V
> directamente. Si el módulo opera a 5V, agregar un divisor resistivo en VRX y VRY.

---

## Configuración en STM32CubeMX

### 1. Reloj del sistema

En **Clock Configuration**:
- Fuente: HSE → PLL → SYSCLK = **72 MHz**
- HCLK = 72 MHz, APB1 = 36 MHz, APB2 = 72 MHz
- **ADC Prescaler**: `/6` → reloj ADC = 12 MHz (máximo permitido: 14 MHz)

### 2. ADC1 (ejes X e Y)

En **Analog → ADC1**:

- **Mode → IN0**: habilitar como *Regular_Channel* (configura PA0 en modo Analógico automáticamente)
- En **Configuration → Parameter Settings**:

| Parámetro | Valor |
|---|---|
| Data Alignment | Right alignment |
| Scan Conversion Mode | Disabled |
| Continuous Conversion Mode | Disabled |
| Discontinuous Conversion Mode | Disabled |
| Number Of Conversion | 1 |
| Rank 1 → Channel | Channel 0 |
| Rank 1 → Sampling Time | 28 Cycles |
| External Trigger Conv Edge | None |

> **Importante:** La librería reconfigura el canal ADC dinámicamente (sin DMA ni
> Scan). CubeMX solo necesita inicializar ADC1 en modo Single Conversion.

- Para que **PA1 (VRY)** quede en modo analógico, ir a **GPIO** y configurar PA1 como:
  - GPIO Mode: `Analog`
  - No Pull-up/down: `No pull-up and no pull-down`

  *(O bien: agregar IN1 como Rank 2 en ADC1 para que CubeMX gestione el pin
  automáticamente. La librería igualmente ignorará el orden de la secuencia.)*

- En **NVIC Settings** del ADC: **no habilitar** ninguna interrupción (el driver usa polling).

### 3. GPIO para botones

Para el botón **SW (PB5)**:
- Ir a **GPIO → PB5**
- GPIO Mode: `Input mode`
- GPIO Pull-up/Pull-down: `Pull-up`
- User Label (opcional): `JOY_SW`

Repetir para cada botón adicional (PB4, PB3, etc.).

> El pull-up interno hace que la línea esté en 3.3V en reposo. Al presionar
> el botón (que conecta a GND), la línea cae a 0V → `GPIO_PIN_RESET` = presionado.
> Por eso `active_state = GPIO_PIN_RESET` en la configuración del driver.

### 4. FreeRTOS

En **Middleware → FreeRTOS**:
- Interface: **FreeRTOS** (sin CMSIS-RTOS, para usar `vTaskDelay` directamente)
- Heap Size: mínimo **6144 bytes** (cubre la tarea del driver + la de aplicación + cola)
- No crear tareas desde CubeMX — se crean manualmente en `main.c`

### 5. USART2 (opcional, para el ejemplo con UART)

- **Connectivity → USART2** → Asynchronous, Baud Rate: 115200
- El Nucleo F103RB tiene USART2 conectado al ST-Link virtual COM port (aparece
  como puerto COM en la PC al conectar USB).

---

## Integración en el proyecto CubeIDE

### Paso 1 — Agregar los archivos al proyecto

1. Copiar `Inc/joystick_yxa197.h` a `Core/Inc/` del proyecto.
2. Copiar `Src/joystick_yxa197.c` a `Core/Src/` del proyecto.
3. CubeIDE los incluye automáticamente en la compilación (no requiere ajustar rutas de include).

### Paso 2 — Incluir el header en main.c

```c
/* USER CODE BEGIN Includes */
#include "joystick_yxa197.h"
/* USER CODE END Includes */
```

### Paso 3 — Crear la cola y configurar el driver en main()

```c
/* USER CODE BEGIN PV */
static QueueHandle_t xJoyQueue;
/* USER CODE END PV */

/* USER CODE BEGIN 2 */

// Crear la cola ANTES de Joystick_Init
xJoyQueue = xQueueCreate(8U, sizeof(JoystickEvent_t));

// Configurar el driver
JoystickConfig_t cfg;
Joystick_GetDefaultConfig(&cfg);
cfg.hadc      = &hadc1;
cfg.x_channel = ADC_CHANNEL_0;           // VRX → PA0
cfg.y_channel = ADC_CHANNEL_1;           // VRY → PA1

cfg.buttons[JOY_BTN_SW].port         = GPIOB;
cfg.buttons[JOY_BTN_SW].pin          = GPIO_PIN_5;
cfg.buttons[JOY_BTN_SW].active_state = GPIO_PIN_RESET; // activo bajo (pull-up)
cfg.button_count  = 1;
cfg.event_queue   = xJoyQueue;

Joystick_Init(&cfg);

// Crear tarea de aplicación (la que consume los eventos)
xTaskCreate(vJoystickAppTask, "JoyApp", 256, NULL, 3, NULL);

vTaskStartScheduler();
/* USER CODE END 2 */
```

### Paso 4 — Tarea de aplicación

Ver `Examples/joystick_example.c`, función `vJoystickAppTask`, para el código
completo de la tarea que consume eventos de la cola.

---

## API resumida

```c
// Rellenar defaults (luego asignar hadc, channels, buttons, event_queue)
void Joystick_GetDefaultConfig(JoystickConfig_t *config);

// Inicializar driver y crear tarea interna. Retorna pdPASS o pdFAIL.
BaseType_t Joystick_Init(const JoystickConfig_t *config);

// Consultar estado actual sin pasar por la cola
JoystickDirection_t Joystick_GetDirection(void);
void                Joystick_GetRaw(uint16_t *x, uint16_t *y);
uint8_t             Joystick_IsButtonPressed(JoystickButtonId_t btn);

// Debug: convertir dirección a string ("ARRIBA", "DERECHA", etc.)
const char *Joystick_DirectionToString(JoystickDirection_t dir);
```

---

## Arquitectura interna

```
 Tarea vJoystickTask (período fijo, vTaskDelayUntil)
    │
    ├─► prv_adc_read(hadc1, CH0) → X_raw    [~3.4 µs]
    ├─► prv_adc_read(hadc1, CH1) → Y_raw    [~3.4 µs]
    │
    ├─► prv_calc_direction(X, Y) → dir
    │       Si dir ≠ dir_anterior → xQueueSend(JOY_EVENT_DIRECTION_CHANGE)
    │
    └─► Para cada botón configurado:
            HAL_GPIO_ReadPin() → raw_pressed
            Actualizar contador de debounce
            Si estado estable cambió → xQueueSend(JOY_EVENT_BUTTON_PRESS/RELEASE)

 Cola xJoyQueue (JoystickEvent_t)
    │
    └─► vJoystickAppTask (tarea del usuario)
            xQueueReceive(portMAX_DELAY) → procesar evento
```

---

## Calibración del umbral de dirección

El threshold (por defecto 1500 de 4095) determina cuánto hay que mover el
joystick para registrar una dirección. Para calibrarlo:

1. Habilitar la tarea `vJoystickCalibTask` del ejemplo (imprime X, Y por UART).
2. Con el joystick en reposo, verificar que X ≈ Y ≈ 2048 (±200 de variación es normal).
3. Mover el joystick hasta el extremo de cada eje y anotar el valor máximo/mínimo.
4. Ajustar el threshold a ~60–70% de la desviación máxima observada.

Ejemplo: si el máximo X es 3900 → desviación = 3900 − 2048 = 1852 → threshold = 1200.

Si las direcciones son opuestas a las esperadas (arriba da ABAJO), activar el flag
correspondiente:
```c
cfg.invert_y = 1;   // o invert_x = 1
```

---

## Recursos consumidos

| Recurso | Cantidad |
|---|---|
| ADC | 1 (ADC1, dos canales leídos secuencialmente) |
| GPIO Input | 1–4 (botón SW + opcionales) |
| ISRs | Ninguna (driver completamente por polling) |
| Tarea FreeRTOS | 1 interna ("Joystick", prio 2, 1 KB stack) |
| Cola FreeRTOS | 1 (creada por el usuario, 8 × 12 bytes ≈ 96 bytes) |
| RAM driver (estado) | ~60 bytes |
| RAM tarea (stack) | 256 palabras × 4 bytes = 1024 bytes |

---

## Notas importantes

### Prioridades de interrupción y FreeRTOS

El driver no usa ISRs, por lo que no hay restricciones de prioridad de interrupción
relacionadas con FreeRTOS en esta librería. Configurar las ISRs de otros periféricos
(UART, TIM, etc.) con prioridad ≥ 5 si usan la API `FromISR` de FreeRTOS.

### Propiedad exclusiva del ADC

La tarea interna del driver llama a `HAL_ADC_ConfigChannel` en cada período, lo que
reconfigura el hardware del ADC1. **Ningún otro módulo debe usar `hadc1` mientras el
driver esté activo.** Si se necesita leer otros canales ADC (ej. temperatura, batería),
usar ADC2 o coordinar el acceso con un mutex.

### Tiempo de debounce vs. período de polling

El debounce efectivo es `JOYSTICK_DEBOUNCE_COUNT × poll_period_ms`. Con los valores
por defecto (3 × 20 ms = 60 ms) se filtran correctamente los rebotes mecánicos típicos.
Para botones de mala calidad, aumentar `JOYSTICK_DEBOUNCE_COUNT` a 5 (100 ms).

### Regeneración de código por CubeMX

Al regenerar el proyecto, los archivos `joystick_yxa197.h` y `joystick_yxa197.c`
no son modificados por CubeMX. Solo verificar que el código en las zonas
`USER CODE BEGIN/END` de `main.c` se haya preservado correctamente.

### Heap FreeRTOS mínimo recomendado

Con este driver más una tarea de aplicación y la cola:
- Driver task (256 words): ~1 KB stack + ~100 bytes TCB ≈ 1.1 KB
- App task (256 words): ~1 KB stack + ~100 bytes TCB ≈ 1.1 KB
- Cola (8 × 12 bytes): ~100 bytes + estructura FreeRTOS ≈ 200 bytes
- **Total mínimo sugerido: 6144 bytes** (`configTOTAL_HEAP_SIZE` en FreeRTOSConfig.h)
