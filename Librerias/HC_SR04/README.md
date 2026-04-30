# HC-SR04 — Driver FreeRTOS para STM32

Driver para el sensor ultrasónico HC-SR04 diseñado para FreeRTOS en STM32,
usando únicamente la HAL de STM32 (`vTaskDelay`/`vTaskDelayUntil`, sin
`osDelay` ni capa CMSIS-RTOS). Desarrollado y probado sobre la **Nucleo
STM32F103RB** con STM32CubeIDE + STM32CubeMX.

---

## Archivos

```
HC_SR04/
├── Inc/
│   └── hcsr04.h          API pública, tipos y macros
├── Src/
│   └── hcsr04.c          Implementación del driver
├── Examples/
│   └── hcsr04_example.c  Ejemplo comentado de integración
└── README.md             Este archivo
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
- Sensor **HC-SR04** (alimentación 5 V, señales 5 V tolerantes)
- **Divisor resistivo** en la línea ECHO (el STM32 tolera 3.3 V en GPIO):
  - ECHO_sensor → R1 (10 kΩ) → PA0 (MCU)
  - PA0 (MCU) → R2 (20 kΩ) → GND
  - Esto convierte los 5 V del sensor a ~3.3 V para el MCU.
- Cable de conexión / breadboard

### Conexiones de ejemplo

| HC-SR04 | STM32 Nucleo F103RB |
|---------|---------------------|
| VCC     | 5V (pin CN6)        |
| GND     | GND (pin CN6)       |
| TRIG    | PA8 (CN9-8)         |
| ECHO    | PA0 via divisor (CN7-28) → TIM2_CH1 |

> El pin ECHO **no es 5V-tolerante** en todos los STM32F1. Siempre usar
> el divisor resistivo para proteger el MCU.

---

## Configuración en STM32CubeMX

### 1. Reloj del sistema

En **Clock Configuration**:
- Fuente: HSE → PLL → SYSCLK = **72 MHz**
- HCLK = 72 MHz, APB1 = 36 MHz, APB2 = 72 MHz
- El timer APB1 (TIM2–TIM4) recibe el doble del APB1 = **72 MHz**

### 2. Pin TRIG (GPIO Output)

- Pin: `PA8` (o el que prefieras, debe ser GPIO estándar)
- Mode: **Output Push Pull**
- Pull-up/Pull-down: **No pull-up/down**
- Maximum output speed: **Low**
- User Label (opcional): `TRIG`

### 3. Timer ECHO (Input Capture)

- Periférico: **TIM2**
- Channel 1: **Input Capture direct mode**
  - Polarity: **Rising Edge**
  - IC Selection: Direct
  - Prescaler: 0 (sin prescaler de canal)
  - Input Filter: 0
- Configuración del timer:
  - **Prescaler**: `71`  → clock timer = 72 MHz / (71+1) = **1 MHz → 1 µs/tick**
  - **Counter Period (ARR)**: `65535`  → máximo 65.5 ms sin overflow
  - Counter Mode: Up
  - Clock Division: No Division
- En **NVIC Settings** (dentro de TIM2):
  - Habilitar: **TIM2 global interrupt** ✓
  - Priority: `5` (más bajo que el kernel FreeRTOS, que usa por defecto 15)

> El pin PA0 queda asignado automáticamente a TIM2_CH1 por CubeMX.

### 4. FreeRTOS

- En **Middleware → FreeRTOS**:
  - Interface: **FreeRTOS** (no CMSIS-RTOS, para usar vTaskDelay directamente)
  - O bien CMSIS-RTOS v1/v2: ambas funcionan, pero en el código se llama
    `vTaskDelay`/`vTaskDelayUntil` directamente, no `osDelay`.
- Crear al menos una tarea (o crearla manualmente con `xTaskCreate` en main.c).
- Heap size: mínimo **4096 bytes** para cubrir stacks y semáforos.

### 5. UART (opcional, para depuración)

- **USART2** → Asynchronous, 115200 baud.
- Se usa en el ejemplo para imprimir la distancia por puerto serie.
- El Nucleo F103RB tiene USART2 conectado al ST-Link (aparece como COM en PC).

---

## Integración en el proyecto CubeIDE

### Paso 1 — Agregar los archivos al proyecto

1. Copiar `Inc/hcsr04.h` a la carpeta `Core/Inc/` del proyecto.
2. Copiar `Src/hcsr04.c` a la carpeta `Core/Src/` del proyecto.
3. CubeIDE los incluirá automáticamente en la compilación.

### Paso 2 — Include en main.c

```c
/* USER CODE BEGIN Includes */
#include "hcsr04.h"
/* USER CODE END Includes */
```

### Paso 3 — Declaración global del handle

```c
/* USER CODE BEGIN PV */
HCSR04_Handle_t hsonar;
/* USER CODE END PV */
```

### Paso 4 — Inicialización en main()

```c
/* USER CODE BEGIN 2 */
HCSR04_Init(&hsonar, GPIOA, GPIO_PIN_8, &htim2, TIM_CHANNEL_1);

xTaskCreate(vSonarTask, "Sonar", 256, NULL, 2, NULL);

vTaskStartScheduler();
/* USER CODE END 2 */
```

### Paso 5 — Callback de Input Capture

```c
/* USER CODE BEGIN 4 */
void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim)
{
    HCSR04_CaptureCallback(&hsonar, htim);
}
/* USER CODE END 4 */
```

### Paso 6 — Tarea de medición

Ver `Examples/hcsr04_example.c` para el código completo de `vSonarTask`.

---

## Uso mínimo de la API

```c
HCSR04_Handle_t hsonar;
float distancia;

// Inicializar (una sola vez)
HCSR04_Init(&hsonar, GPIOA, GPIO_PIN_8, &htim2, TIM_CHANNEL_1);

// En la tarea FreeRTOS
for (;;) {
    if (HCSR04_Measure(&hsonar, &distancia) == HCSR04_OK) {
        // distancia en centímetros
    }
    vTaskDelay(pdMS_TO_TICKS(200));
}
```

---

## Recursos consumidos

| Recurso | Cantidad |
|---|---|
| GPIO Output | 1 (TRIG) |
| Timer 16-bit | 1 (TIM2 o TIM3/4) |
| Canal Input Capture | 1 |
| Interrupción NVIC | 1 (TIM global) |
| Semáforo FreeRTOS | 1 por sensor |
| RAM (handle) | ~40 bytes por sensor |
| RAM (semáforo FreeRTOS heap) | ~80 bytes |

---

## Notas importantes

### Prioridades de interrupción y FreeRTOS

FreeRTOS en STM32 requiere que todas las ISRs que usen la API `FromISR`
tengan prioridad **≥ configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY** (en
CubeMX suele ser 5). La interrupción de TIM2 debe tener prioridad **5 o
mayor** (número mayor = menor urgencia en ARM). Prioridades 0-4 son
reservadas para ISRs que no llaman a la API de FreeRTOS.

### Interferencia entre sensores

Si usás más de un HC-SR04, **nunca dispares dos al mismo tiempo**. Los
pulsos ultrasónicos a 40 kHz interfieren entre sí. Agregar un retardo
de al menos **60 ms entre mediciones** de distintos sensores.

### Temperatura y velocidad del sonido

La constante `HCSR04_SOUND_SPEED = 0.0343 cm/µs` corresponde a 20 °C.
A otras temperaturas: `v = 0.03313 + 0.0000606 × T_celsius` cm/µs.

### Regeneración de código por CubeMX

Al regenerar el proyecto con CubeMX, los archivos `hcsr04.h` y `hcsr04.c`
no son tocados (no los genera Cube). Solo verificar que el código en las
zonas `USER CODE` de `main.c` se haya preservado.

---

## Diagrama de flujo de una medición

```
Tarea vSonarTask
    │
    ├─► HCSR04_Measure()
    │       ├─ Arma IC en flanco subida
    │       ├─ Pulso TRIG 12 µs (DWT busy-wait)
    │       └─ xSemaphoreTake() → TAREA SUSPENDIDA
    │
    │   [ECHO sube — ISR dispara]
    │       ├─ Guarda t_rise
    │       └─ Cambia polaridad a bajada
    │
    │   [ECHO baja — ISR dispara]
    │       ├─ Guarda t_fall
    │       ├─ Detiene IC
    │       └─ xSemaphoreGiveFromISR() → TAREA DESPIERTA
    │
    └─► Calcula d = (t_fall - t_rise) × 0.0343 / 2
```
