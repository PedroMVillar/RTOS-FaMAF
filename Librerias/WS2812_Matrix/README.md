# WS2812B Matrix 8x8 — Driver FreeRTOS para STM32

Driver para matriz NeoPixel 8x8 WS2812B usando **Timer PWM + DMA** en STM32,
totalmente compatible con FreeRTOS. La tarea se suspende durante la
transmisión (`vTaskDelay` implícito vía semáforo); el CPU queda libre para
otras tareas mientras los 1536 bits se envían por DMA.

Plataforma validada: **STM32 Nucleo F103RB** con STM32CubeIDE + STM32CubeMX.

---

## Archivos

```
WS2812_Matrix/
├── Inc/
│   └── ws2812.h          API pública, tipos, macros configurables
├── Src/
│   └── ws2812.c          Implementación (build DMA buffer, HSV, etc.)
├── Examples/
│   └── ws2812_example.c  Bitmaps, animaciones, ejemplo con HC-SR04
└── README.md             Este archivo
```

---

## Requisitos de software

| Componente | Versión mínima |
|---|---|
| STM32CubeIDE | 1.13 |
| STM32CubeF1 HAL firmware | 1.8.5 |
| FreeRTOS | 10.x (incluido en CubeMX) |

---

## Requisitos de hardware

- **STM32 Nucleo F103RB**
- **Matriz WS2812B 8x8** (p.ej. Adafruit 8x8 NeoPixel Matrix)
- **Fuente de alimentación externa de 5V** capaz de entregar al menos:
  - Mínimo: 64 LEDs × 20 mA = 1.28 A (todos en blanco al 100%)
  - Recomendado: 2 A para margen de seguridad
- **Resistencia de 300–500 Ω** en serie en la línea DATA (protege el primer LED)
- **Capacitor de 100–1000 µF** en paralelo en la alimentación de la matriz
  (filtra picos de corriente)
- **Nivel lógico**: La línea DATA del STM32 es 3.3 V; el WS2812B acepta
  señal de 3.3 V en la mayoría de los casos, pero si hay problemas de
  reconocimiento se puede agregar un level shifter 3.3V→5V.

### Conexiones

| WS2812B Matrix | STM32 Nucleo F103RB |
|----------------|---------------------|
| 5V             | Fuente externa 5V (NO usar el 5V del Nucleo para la matriz) |
| GND            | GND común con el Nucleo |
| DIN            | 300Ω → PA1 (TIM2_CH2, CN7-30) |

> Conectar el GND de la fuente externa al GND del Nucleo para tener
> referencia común.

---

## Configuración en STM32CubeMX

### 1. Reloj del sistema

En **Clock Configuration**: SYSCLK = **72 MHz** (HSE → PLL).
Los timers en APB1 (TIM2–TIM4) reciben 72 MHz (APB1 × 2).

### 2. Timer TIM2 — PWM + DMA

En **Timers → TIM2**:

| Parámetro | Valor |
|---|---|
| Prescaler | `0` (timer a 72 MHz) |
| Counter Period (ARR) | `89` |
| Counter Mode | Up |
| Channel 2 | PWM Generation CH2 |
| CH2 Mode | PWM mode 1 |
| CH2 Pulse (CCR2) | `0` |
| CH2 Output Compare Polarity | High |
| CH2 Fast Mode | Disable |

El pin PA1 queda asignado automáticamente como TIM2_CH2.

**DMA Settings (dentro de TIM2):**

| Campo | Valor |
|---|---|
| DMA Request | TIM2_CH2 |
| Channel | DMA1 Channel 7 (asignado automáticamente) |
| Direction | Memory To Peripheral |
| Priority | High |
| Mode | Normal |
| Data Width (Peripheral) | Half Word |
| Data Width (Memory) | Half Word |
| Increment Address | Memory (solo memoria incrementa) |

**NVIC Settings (dentro de TIM2):**
- ☑ TIM2 global interrupt — prioridad **`5`**

**NVIC (global, en System Core → NVIC):**
- ☑ DMA1 channel7 global interrupt — prioridad **`5`**

> Prioridad 5 es el mínimo para llamar a la API `FromISR` de FreeRTOS
> (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5 por defecto en CubeMX).

### 3. FreeRTOS

- Middleware → FreeRTOS → **ENABLED**
- Heap Size: mínimo **`6144`** bytes (stacks + semáforos + handle ~3.5 KB)
- TOTAL_HEAP_SIZE recomendado: **8192** bytes

### 4. ¿Conflicto con HC-SR04?

**Sí hay conflicto.** Esta configuración usa TIM2_CH2 (PA1). El driver
HC-SR04 también ocupa TIM2 para Input Capture, por lo que **no se pueden
usar ambos en el mismo proyecto**.

Para combinar WS2812 + HC-SR04, reasignar el WS2812 a:
- **TIM3_CH1** → PA6, DMA1_Channel6 (opción recomendada), o
- **TIM4_CH1** → PB6, DMA1_Channel1

---

## Integración en el proyecto CubeIDE

### Paso 1 — Copiar archivos

1. `Inc/ws2812.h` → `Core/Inc/`
2. `Src/ws2812.c` → `Core/Src/`

### Paso 2 — main.c: include y handle global

```c
/* USER CODE BEGIN Includes */
#include "ws2812.h"
/* USER CODE END Includes */

/* USER CODE BEGIN PV */
WS2812_Handle_t hmatrix;   /* GLOBAL, no en stack de tarea */
/* USER CODE END PV */
```

### Paso 3 — main.c: inicialización y tarea

```c
/* USER CODE BEGIN 2 */
WS2812_Init(&hmatrix, &htim2, TIM_CHANNEL_2);

xTaskCreate(vMatrixTask, "Matrix", 512, NULL, 2, NULL);

vTaskStartScheduler();
/* USER CODE END 2 */
```

### Paso 4 — main.c: callback de DMA

```c
/* USER CODE BEGIN 4 */
void HAL_TIM_PWM_PulseFinishedCallback(TIM_HandleTypeDef *htim)
{
    WS2812_DMA_Callback(&hmatrix, htim);
}
/* USER CODE END 4 */
```

---

## API de referencia rápida

```c
// Inicialización
WS2812_Init(&hmatrix, &htim2, TIM_CHANNEL_2);
WS2812_SetBrightness(&hmatrix, 80);          // 0–255, default 255

// Escritura en buffer (no envía todavía)
WS2812_SetPixel(&hmatrix, row, col, r, g, b);
WS2812_SetPixelColor(&hmatrix, row, col, WS2812_RED);
WS2812_Fill(&hmatrix, 255, 128, 0);
WS2812_FillColor(&hmatrix, WS2812_CYAN);
WS2812_Clear(&hmatrix);
WS2812_SetRow(&hmatrix, 3, 0, 255, 0);
WS2812_SetCol(&hmatrix, 7, 0, 0, 255);
WS2812_SetBitmap(&hmatrix, bitmap, WS2812_RED, WS2812_BLACK);

// Conversión de color
WS2812_Color_t c = WS2812_HSV(128, 255, 200);   // turquesa
WS2812_Color_t m = WS2812_Lerp(WS2812_RED, WS2812_BLUE, 128);  // violeta

// Envío a los LEDs (tarea se suspende hasta completar)
WS2812_Refresh(&hmatrix);   // llamar siempre desde una tarea FreeRTOS
```

---

## Consumo de recursos

| Recurso | Cantidad |
|---|---|
| Timer 16-bit | 1 (TIM2) |
| Canal DMA | 1 (DMA1_Ch7) |
| Interrupciones NVIC | 2 (TIM2 global + DMA1_Ch7) |
| Semáforo FreeRTOS | 1 |
| RAM — buffer DMA (`uint16_t`) | 1596 × 2 = **3192 bytes** |
| RAM — buffer de píxeles | 8×8×3 = **192 bytes** |
| RAM — resto del handle | ~20 bytes |
| **RAM total del handle** | **~3.4 KB** |
| Flash (código) | ~2.5 KB |

> El handle debe ser una **variable global** o estar en RAM estática.
> No declararlo en el stack de ninguna tarea.

---

## Temporización

```
              ╔══════════╗
 Bit '1':     ║ 0.8 µs   ║ 0.45 µs (bajo)   → CCR = 58, ARR = 89
              ╚══════════╝

 Bit '0': ╗ 0.4 µs ╔══════════════════╗ 0.85 µs (bajo)  → CCR = 29, ARR = 89
          ╚════════╝

 Reset:   ═════════════════════════════════════════════  > 50 µs (bajo)
          (60 períodos × 1.25 µs = 75 µs)

 Tiempo de transmisión de 64 LEDs:
   1536 bits × 1.25 µs/bit = 1.92 ms de datos
   + 75 µs de reset
   = ~2 ms total por llamada a WS2812_Refresh()
```

---

## Cableado serpentina (más común)

```
  col:  0    1    2    3    4    5    6    7
row 0: [0]→ [1]→ [2]→ [3]→ [4]→ [5]→ [6]→ [7]
                                              ↓
row 1: [15]←[14]←[13]←[12]←[11]←[10]←[9]← [8]
↓
row 2: [16]→[17]→[18]→[19]→[20]→[21]→[22]→[23]
                                              ↓
                          ...
```

La API usa siempre coordenadas lógicas: `col=0` = columna izquierda.
Si tu matriz no es serpentina, cambiar `WS2812_SERPENTINE 0` en `ws2812.h`.

---

## Notas importantes

### Alimentación — punto crítico

Los WS2812B consumen hasta **60 mA por LED** (20 mA por canal RGB). Con 64
LEDs en blanco al 100%: **3.84 A**. Nunca alimentar la matriz desde el pin
5V del Nucleo (máximo 500 mA del USB). Usar **fuente externa de 5V/2A mínimo**
con GND común al Nucleo.

### Primer período fantasma

El primer período del timer tiene CCR=0 (1.25 µs en bajo) antes de que el
DMA cargue el primer dato. Esto es indetectable por el WS2812B (el reset
requiere > 50 µs). No afecta el funcionamiento.

### Regeneración de CubeMX

Al regenerar el proyecto, los archivos `ws2812.h` y `ws2812.c` no son
modificados por CubeMX. Verificar que el código en las zonas `USER CODE`
de `main.c` se haya preservado.

### Brillo y temperatura de color

A bajo brillo (< 30) los LEDs pueden mostrar variaciones de color visibles
(no-linealidad del PWM de los WS2812B). Para efectos suaves, mantener el
brillo ≥ 30 y reducir los valores RGB en lugar de usar `SetBrightness`.
