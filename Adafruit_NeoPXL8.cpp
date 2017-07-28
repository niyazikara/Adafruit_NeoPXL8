/*
8-way concurrent DMA NeoPixel library for M0-based boards (Feather M0,
Arduino Zero, etc.).  Doesn't require stopping interrupts,
so millis()/micros() don't lose time, soft PWM (for servos, etc.)
still operate normally, etc.  Also, has nondestructive brightness
scaling...unlike classic NeoPixel, getPixelColor() here always returns
the original value as was passed to setPixelColor().

THIS IS A WORK-IN-PROGRESS AND NOT 100% THERE YET.

THIS ONLY WORKS ON CERTAIN PINS.  THIS IS NORMAL.  Library uses TCC0
"pattern generator" peripheral for output, and the hardware only supports
this on specific pins.  See example sketch for explanation.

0/1 bit timing does not precisely match NeoPixel/WS2812/SK6812 datasheet
specs, but it seems to work well enough.  Use at your own peril.

Some of the more esoteric NeoPixel functions are not implemented here,
so this is not a 100% drop-in replacement for all NeoPixel code right now.
*/

#include "Adafruit_NeoPXL8.h"
#include "wiring_private.h" // pinPeripheral() function

// DMA transfer using TCC0 as beat clock seems to stutter on the first
// few elements out, which can botch the delicate NeoPixel timing.
// A few initial zero bytes are issued to give DMA time to stabilize.
// The number of bytes here was determined empirically.
#define EXTRASTARTBYTES 7
// Not a perfect solution and you may still see infrequent glitches,
// especially on the first pixel of a strand.  This is most likely due to
// the 1:3 bit timing generated by this code (close but doesn't exactly
// match the NeoPixel spec)...usually only affects the 1st pixel,
// subsequent pixels OK due to signal reshaping through the 1st.

static const    int8_t   defaultPins[] = { 0,1,2,3,4,5,6,7 };
static volatile boolean  sending = 0;
static volatile uint32_t lastBitTime;

Adafruit_NeoPXL8::Adafruit_NeoPXL8(
  uint16_t n, uint8_t *p, neoPixelType t) : Adafruit_NeoPixel(n * 8, -1, t),
  brightness(256), dmaBuf(NULL) {
  memcpy(pins, p ? p : defaultPins, sizeof(pins));
}

Adafruit_NeoPXL8::~Adafruit_NeoPXL8() {
  dma.abort();
  if(dmaBuf) free(dmaBuf);
}

// This table holds PORTs, bits and peripheral selects of valid pattern
// generator waveform outputs.  This data is not in the Arduino variant
// header...was derived from the SAM D21E/G/J datasheet.  Some of these
// PORT/pin combos are NOT present on some dev boards or SAMD21 variants.
static struct {
  EPortType port;       // PORTA|PORTB
  uint8_t   bit;        // Port bit (0-31)
  uint8_t   wo;         // TCC0/WO# (0-7)
  EPioType  peripheral; // Peripheral to select for TCC0 out
} tcc0pinMap[] = {
  { PORTA,  4, 0, PIO_TIMER     },
  { PORTA,  5, 1, PIO_TIMER     },
  { PORTA,  8, 0, PIO_TIMER     },
  { PORTA,  9, 1, PIO_TIMER     },
  { PORTA, 10, 2, PIO_TIMER_ALT },
  { PORTA, 11, 3, PIO_TIMER_ALT },
  { PORTA, 12, 6, PIO_TIMER_ALT },
  { PORTA, 13, 7, PIO_TIMER_ALT },
  { PORTA, 14, 4, PIO_TIMER_ALT },
  { PORTA, 15, 5, PIO_TIMER_ALT },
  { PORTA, 16, 6, PIO_TIMER_ALT }, // Not in Rev A silicon
  { PORTA, 17, 7, PIO_TIMER_ALT }, // Not in Rev A silicon
  { PORTA, 18, 2, PIO_TIMER_ALT },
  { PORTA, 19, 3, PIO_TIMER_ALT },
  { PORTA, 20, 6, PIO_TIMER_ALT },
  { PORTA, 21, 7, PIO_TIMER_ALT },
  { PORTA, 22, 4, PIO_TIMER_ALT },
  { PORTA, 23, 5, PIO_TIMER_ALT },
  { PORTB, 10, 4, PIO_TIMER_ALT },
  { PORTB, 11, 5, PIO_TIMER_ALT },
  { PORTB, 12, 6, PIO_TIMER_ALT },
  { PORTB, 13, 7, PIO_TIMER_ALT },
  { PORTB, 16, 4, PIO_TIMER_ALT },
  { PORTB, 17, 5, PIO_TIMER_ALT },
  { PORTB, 30, 0, PIO_TIMER     },
  { PORTB, 31, 1, PIO_TIMER     }
};
#define PINMAPSIZE (sizeof(tcc0pinMap) / sizeof(tcc0pinMap[0]))

// Given a pin number, locate corresponding entry in the pin map table
// above, configure as a pattern generator output and return bitmask
// for later data conversion (returns 0 if invalid pin).
static uint8_t configurePin(uint8_t pin) {
  if((pin >= 0) && (pin < PINS_COUNT)) {
    EPortType port = g_APinDescription[pin].ulPort;
    uint8_t   bit  = g_APinDescription[pin].ulPin;
    for(uint8_t i=0; i<PINMAPSIZE; i++) {
      if((port == tcc0pinMap[i].port) && (bit == tcc0pinMap[i].bit)) {
        pinPeripheral(pin, tcc0pinMap[i].peripheral);
        return (1 << tcc0pinMap[i].wo);
      }
    }
  }
  return 0;
}

// Called at end of DMA transfer.  Clears 'sending' flag and notes
// start-of-NeoPixel-latch time.
static void dmaCallback(struct dma_resource* const resource) {
  lastBitTime = micros();
  sending     = 0;
}

boolean Adafruit_NeoPXL8::begin(void) {
  Adafruit_NeoPixel::begin(); // Call base class begin() function 1st
  if(pixels) { // Successful malloc of NeoPixel buffer?
    uint8_t  bytesPerPixel = (wOffset == rOffset) ? 3 : 4;
    uint32_t bytesTotal    = numLEDs * bytesPerPixel * 3 + EXTRASTARTBYTES + 3;
    if((dmaBuf = (uint8_t *)malloc(bytesTotal))) {
      int i;

      dma.configure_peripheraltrigger(TCC0_DMAC_ID_OVF);
      dma.configure_triggeraction(DMA_TRIGGER_ACTON_BEAT);

      // Get address of first byte that's on a 32-bit boundary
      // and at least EXTRASTARTBYTES into dmaBuf...
      alignedAddr = (uint32_t *)((uint32_t)(&dmaBuf[EXTRASTARTBYTES + 3]) & ~3);
      // DMA transfer then starts EXTRABYTES back from this to stabilize
      uint8_t *startAddr = (uint8_t *)alignedAddr - EXTRASTARTBYTES;
      memset(startAddr, 0, EXTRASTARTBYTES); // Initialize start with zeros

      uint8_t *dst = &((uint8_t *)(&TCC0->PATT))[1]; // PAT.vec.PGV
      dma.allocate();
      dma.setup_transfer_descriptor(
        startAddr,          // source
        dst,                // destination
        bytesTotal - 3,     // count (don't include long-alignment bytes!)
        DMA_BEAT_SIZE_BYTE, // size per
        true,               // increment source
        false);             // don't increment destination
      dma.add_descriptor();

      dma.register_callback(dmaCallback);
      dma.enable_callback();

      // Enable GCLK for TCC0
      GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_CLKEN |
        GCLK_CLKCTRL_GEN_GCLK0 | GCLK_CLKCTRL_ID(GCM_TCC0_TCC1));
      while(GCLK->STATUS.bit.SYNCBUSY == 1);

      // Disable TCC before configuring it
      TCC0->CTRLA.bit.ENABLE = 0;
      while(TCC0->SYNCBUSY.reg & TCC_SYNCBUSY_MASK);

      TCC0->CTRLA.reg = TCC_CTRLA_PRESCALER_DIV1; // 1:1 Prescale
      while(TCC0->SYNCBUSY.reg & TCC_SYNCBUSY_MASK);

      TCC0->WAVE.bit.WAVEGEN = TCC_WAVE_WAVEGEN_NPWM_Val; // Normal PWM mode
      while(TCC0->SYNCBUSY.reg & TCC_SYNCBUSY_MASK);

      TCC0->CC[0].reg = 0; // No PWM out
      while(TCC0->SYNCBUSY.reg & TCC_SYNCBUSY_MASK);

      // 2.4 GHz clock: 3 DMA xfers per NeoPixel bit = 800 KHz
      TCC0->PER.reg = ((F_CPU + 1200000)/ 2400000) - 1;
      while(TCC0->SYNCBUSY.reg & TCC_SYNCBUSY_MASK);

      memset(bitmask, 0, sizeof(bitmask));
      uint8_t enableMask = 0x00;       // Bitmask of pattern gen outputs
      for(i=0; i<8; i++) {
        if(bitmask[i] = configurePin(pins[i])) enableMask |= 1 << i;
      }
      TCC0->PATT.vec.PGV = 0;          // Set all pattern outputs to 0
      TCC0->PATT.vec.PGE = enableMask; // Enable pattern outputs

      TCC0->CTRLA.bit.ENABLE = 1;
      while(TCC0->SYNCBUSY.reg & TCC_SYNCBUSY_MASK);

      return true; // Success!
    }
    free(pixels);
    pixels = NULL;
  }

  return false;
}

// Convert NeoPixel buffer to NeoPXL8 output format
void Adafruit_NeoPXL8::stage(void) {

  uint8_t  bytesPerLED  = (wOffset == rOffset) ? 3 : 4;
  uint32_t pixelsPerRow = numLEDs / 8,
           bytesPerRow  = pixelsPerRow * bytesPerLED,
           i;

  static const uint8_t dmaFill[] __attribute__ ((__aligned__(4))) = {
    0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00,
    0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00
  };

  // Clear DMA buffer data
  uint32_t *in  = (uint32_t *)dmaFill,
           *out = alignedAddr;
  for(i=0; i<bytesPerRow; i++) {
    *out++ = in[0]; *out++ = in[1]; *out++ = in[2];
    *out++ = in[3]; *out++ = in[4]; *out++ = in[5];
  }

  for(uint8_t b=0; b<8; b++) { // For each output pin 0-7
    if(bitmask[b]) { // Enabled?
      uint8_t *src = &pixels[b * bytesPerRow]; // Start of row data
      uint8_t *dst = &((uint8_t *)alignedAddr)[1];
      for(i=0; i<bytesPerRow; i++) { // Each byte in row...
        // Brightness scaling doesn't require shift down,
        // we'll just pluck from bits 15-8...
        uint16_t value = *src++ * brightness;
        for(uint16_t foo=0x8000; foo >= 0x0100; foo >>= 1) {
          if(value & foo) *dst |= bitmask[b];
          dst += 3;
        }
      }
    }
  }

  staged = true;
}

void Adafruit_NeoPXL8::show(void) {
  while(sending);      // Wait for DMA callback
  if(!staged) stage(); // Convert data
  dma.start_transfer_job();
  staged  = false;
  sending = 1;
  while((micros() - lastBitTime) <= 300); // Wait for latch
  dma.trigger_transfer();
}

// Returns true if DMA transfer is NOT presently occurring.
// We MAY (or not) be in the 300 uS EOD latch time, or might be idle.
// Either way, it's now safe to stage data from NeoPixel to DMA buffer.
// This might be helpful for code that wants more precise and uniform
// animation timing...it might be using a timer interrupt or micros()
// delta for frame-to-frame intervals...after calculating the next frame,
// one can 'stage' the data (convert it from NeoPixel buffer format to
// DMA parallel output format) once the current frame has finished
// transmitting, rather than being done at the beginning of the show()
// function (the staging conversion isn't entirely deterministic).
boolean Adafruit_NeoPXL8::canStage(void) {
  return !sending;
}

// Returns true if DMA transfer is NOT presently occurring and
// NeoPixel EOD latch has fully transpired; library is idle.
boolean Adafruit_NeoPXL8::canShow(void) {
  return !sending && ((micros() - lastBitTime) > 300);
}

// Brightness is stored differently here than in normal NeoPixel library.
// In either case it's *specified* the same: 0 (off) to 255 (brightest).
// Classic NeoPixel rearranges this internally so 0 is max, 1 is off and
// 255 is just below max...it's a decision based on how fixed-point math
// is handled in that code.  Here it's stored internally as 1 (off) to
// 256 (brightest), requiring a 16-bit value.

void Adafruit_NeoPXL8::setBrightness(uint8_t b) {
  brightness = (uint16_t)b + 1; // 0-255 in, 1-256 out
}

uint8_t Adafruit_NeoPXL8::getBrightness(void) const {
  return brightness - 1; // 1-256 in, 0-255 out
}
