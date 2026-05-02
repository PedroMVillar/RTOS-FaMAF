# OV7670 — Driver FreeRTOS para STM32 Nucleo F103RB

Driver para captura de imagen con el módulo OV7670 sobre **STM32F103RB** usando **HAL** y **FreeRTOS**. Emplea TIM2 + DMA1 para capturar líneas horizontales sin carga de CPU, y un semáforo binario para sincronizar el frame completo con el task FreeRTOS.

---

## Hardware requerido

| Componente | Detalle |
|---|---|
| Módulo OV7670 | Versión sin FIFO. Con FIFO el pinout es distinto. |
| Resistencias 4.7 kΩ × 2 | Pull-ups en SCCB_SCL y SCCB_SDA a 3.3 V (**obligatorio**) |
| Fuente 3.3 V estable | El OV7670 es sensible a ruido en VCC. Agregar 100 nF + 10 µF en los pines de alimentación del módulo. |

> **Atención**: el STM32F103RB **no tiene DCMI**. La captura se realiza leyendo el puerto GPIO por DMA, accionado por el Input Capture de TIM2.

---

## Conexionado — Nucleo F103RB

```
OV7670 pin   STM32 pin   Señal           Notas
───────────  ──────────  ──────────────  ─────────────────────────────────
D0           PC0         Dato bit 0      D0–D7 deben ir al nibble bajo del
D1           PC1         Dato bit 1      mismo puerto GPIO (PC0–PC7).
D2           PC2         Dato bit 2
D3           PC3         Dato bit 3
D4           PC4         Dato bit 4
D5           PC5         Dato bit 5
D6           PC6         Dato bit 6
D7           PC7         Dato bit 7
PCLK         PA0         Pixel clock     TIM2_CH1 Input Capture
VSYNC        PA2         Sincronismo V   EXTI2, flanco descendente
HREF         —           (no conectar)   PCLK está gateado por HREF en HW
XCLK         PA8         Clock entrada   MCO = HSE = 8 MHz
RESET        PB0         Reset activo L  GPIO Output, init HIGH
PWDN         PB1         Power down H    GPIO Output, init LOW
SIOC/SCL     PB6         SCCB clock      GPIO Open-Drain + 4.7 kΩ a 3.3 V
SIOD/SDA     PB7         SCCB data       GPIO Open-Drain + 4.7 kΩ a 3.3 V
VCC          3.3 V       Alimentación    100 nF + 10 µF cerca del módulo
GND          GND
```

> **HREF**: no es necesario conectarla al MCU. El registro `COM10_PCLK_GATE_HREF` del OV7670 hace que PCLK solo pulse durante HREF alto, por lo que el DMA recibe automáticamente solo píxeles válidos.

---

## Configuración en STM32CubeMX

### 1. RCC — Reloj de entrada al OV7670 (XCLK)

- **System Core → RCC → Master Clock Output (MCO)** = `HSE`
- PA8 queda configurado como MCO automáticamente.
- El OV7670 recibirá 8 MHz en su pin XCLK. Verificar con osciloscopio o analizador lógico.

### 2. TIM2 — Captura de pixel clock

- **Timers → TIM2 → Channel 1** = `Input Capture direct mode`
- Pin: PA0 (asignado automáticamente)
- Prescaler: 0, Period (ARR): 0xFFFFFFFF
- **TIM2 NVIC**: habilitar *TIM2 global interrupt*, prioridad **5** (≥ `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`)
- **DMA Settings** (dentro de TIM2):
  - Agregar canal: `TIM2_CH1`
  - Direction: `Peripheral To Memory`
  - Peripheral Increment: **Disabled**
  - Memory Increment: **Enabled**
  - Peripheral Data Width: **Byte**
  - Memory Data Width: **Byte**
  - Mode: **Normal** (no Circular)
  - Priority: High
- **DMA1 Channel 5 NVIC**: habilitar, misma prioridad que TIM2.

### 3. GPIO

| Pin | Modo | Pull | Velocidad |
|-----|------|------|-----------|
| PC0–PC7 | GPIO Input | No pull | — |
| PA2 | GPIO_EXTI2 | No pull | — |
| PB0 | GPIO Output | No pull | Low |
| PB1 | GPIO Output | No pull | Low |
| PB6 | GPIO Output **Open-Drain** | No pull | Low |
| PB7 | GPIO Output **Open-Drain** | No pull | Low |

- PA2 EXTI: flanco **descendente** (VSYNC bajo = inicio de frame).
- PA2 NVIC: habilitar *EXTI line2 interrupt*, prioridad **5**.

### 4. FreeRTOS

- Interface: **CMSIS_V1** o **CMSIS_V2**
- `configTICK_RATE_HZ` = 1000
- `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` = 5
- Heap: mínimo **4 096 bytes** (para el semáforo + stack del task).

### 5. Generar código

Generar con CubeMX. Los handles `htim2` y `hdma_tim2_ch1` quedan declarados en `main.c`/`main.h` — son necesarios para el campo `.htim` y `.hdma` de `OV7670_Config`.

---

## Integración en el proyecto

### Agregar archivos

```
Core/
  Inc/
    ov7670.h
    ov7670_regs.h
    ov7670_sccb.h
  Src/
    ov7670.c
    ov7670_sccb.c
```

En STM32CubeIDE: *Project → Properties → C/C++ Build → Settings → Includes* — el directorio `Core/Inc` ya suele estar incluido.

### Cabecera STM32

Verificar que `ov7670.h` incluye el header correcto para F103:

```c
#include "stm32f1xx_hal.h"   // F103 — ya está incluido vía main.h
```

### Conectar el callback de VSYNC

En `Core/Src/main.c` (o donde gestiones las EXTI):

```c
/* Declarar extern si hcam está en otro archivo */
extern OV7670_Handle hcam;

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == GPIO_PIN_2) {      /* PA2 = VSYNC */
        OV7670_VSYNC_Callback(&hcam);
    }
}
```

### Crear el task en main()

```c
/* En main(), antes de vTaskStartScheduler() */
OV7670_Example_CreateTask();
vTaskStartScheduler();
```

---

## Uso básico

```c
OV7670_Config cfg = {
    .sccb_scl_port = GPIOB, .sccb_scl_pin = GPIO_PIN_6,
    .sccb_sda_port = GPIOB, .sccb_sda_pin = GPIO_PIN_7,
    .reset_port    = GPIOB, .reset_pin    = GPIO_PIN_0,
    .pwdn_port     = GPIOB, .pwdn_pin     = GPIO_PIN_1,
    .data_port     = GPIOC,
    .htim          = &htim2,
    .tim_channel   = TIM_CHANNEL_1,
    .hdma          = &hdma_tim2_ch1,
    .resolution    = OV7670_RES_QQVGA,
    .format        = OV7670_FMT_RGB565,
    .line_callback = mi_callback_por_linea,  /* puede ser NULL */
    .frame_buffer  = NULL,  /* ver nota de RAM más abajo */
    .frame_buffer_size = 0,
};

OV7670_Handle hcam = {0};

// En el task (después de vTaskStartScheduler):
OV7670_Init(&hcam, &cfg);      // configura HW y sensor
OV7670_StartCapture(&hcam);    // arma DMA

for (;;) {
    if (OV7670_WaitFrame(&hcam, pdMS_TO_TICKS(2000)) == pdTRUE) {
        // frame listo — procesar
        OV7670_StartCapture(&hcam);  // re-armar para el próximo
    }
}
```

### Callback por línea (modo recomendado en F103RB)

```c
void mi_callback_por_linea(const uint8_t *data, uint16_t linea, uint16_t ancho)
{
    // data apunta a 'ancho * 2' bytes de la línea en RGB565
    // Este callback corre en contexto de interrupción DMA — mantenerlo corto.
    // Usar xQueueSendFromISR() para mandar datos a un task de procesamiento.
}
```

---

## Presupuesto de RAM

| Dato | Tamaño |
|------|--------|
| `OV7670_Handle` (incl. line_buffer interno) | ~360 B |
| Frame buffer QQVGA RGB565 completo | 38 400 B ← **NO cabe en F103RB** |
| Frame buffer QQQVGA RGB565 (80×60) | 9 600 B ← factible |
| Frame buffer QQVGA escala de grises | 19 200 B ← ajustado |

**Recomendación para F103RB**: usar el `line_callback` para procesar o transmitir cada línea en tiempo real, sin acumular el frame completo. O bien configurar la ventana del sensor a 80×60 píxeles y alocar 9 600 bytes.

---

## Presupuesto de Flash

| Módulo | Flash estimada |
|--------|---------------|
| `ov7670.c` | ~3 KB |
| `ov7670_sccb.c` | ~1 KB |
| Tablas de registros | ~500 B |

---

## Frecuencias y frame rate

Con XCLK = 8 MHz (MCO/HSE) y la configuración por defecto:

| Parámetro | Valor |
|-----------|-------|
| XCLK | 8 MHz |
| CLK interno (CLKRC=1) | 2 MHz |
| PCLK efectivo (post scaling) | ~1 MHz |
| Frame rate QQVGA estimado | ~6 fps |

Para aumentar fps: reducir `CLKRC` (aumenta CLK interno), verificar que el DMA no sature el bus AHB.

---

## Diagnóstico

| Síntoma | Causa probable |
|---------|---------------|
| `OV7670_Init` retorna `HAL_ERROR` | PID incorrecto: revisar pull-ups SCCB y XCLK |
| Imagen toda negra / blanca | Exposición incorrecta; esperar ~2 s tras init para que AEC converja |
| DMA no dispara | Verificar que PCLK llega a PA0; revisar CubeMX DMA trigger = TIM2_CH1 |
| Frame congelado / incompleto | VSYNC no llega a PA2; verificar COM10 polarity y flanco de EXTI |
| Colores incorrectos en RGB565 | Invertir el orden de bytes en la recepción (swap MSB/LSB por píxel) |

---

## Registros útiles para ajuste en tiempo de ejecución

```c
// Ajustar brillo (0x00 = oscuro, 0x80 = normal, 0xFF = claro)
OV7670_WriteReg(&hcam, OV7670_REG_BRIGHT, 0x80);

// Espejo horizontal
OV7670_WriteReg(&hcam, OV7670_REG_MVFP, MVFP_MIRROR);

// Flip vertical
OV7670_WriteReg(&hcam, OV7670_REG_MVFP, MVFP_FLIP);

// Ambos
OV7670_WriteReg(&hcam, OV7670_REG_MVFP, MVFP_MIRROR | MVFP_FLIP);
```

---

## Estructura de archivos

```
OV7670/
├── Inc/
│   ├── ov7670.h          API principal, tipos, handle
│   ├── ov7670_regs.h     Mapa de registros y constantes
│   └── ov7670_sccb.h     Interfaz SCCB bit-bang
├── Src/
│   ├── ov7670.c          Driver principal, tablas de registros, DMA
│   └── ov7670_sccb.c     Implementación SCCB con DWT timing
├── Examples/
│   └── ov7670_example.c  Ejemplo completo con FreeRTOS task
└── README.md             Este archivo
```
