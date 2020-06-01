//
// Created by robert on 5/27/20.
//

#include <avr/io.h>
#include <avr/interrupt.h>
#include <math.h>
#include "gpsdo.h"
#include "nop.h"
#include "leds.h"
#include "net/dhcp_client.h"

// loop tuning
#define MAX_PPS_DELTA (2) // 32 microseconds
#define SETTLED_VAR (40000) // 1 microsecond RMS
#define RING_SIZE (64u)
#define RING_DIV (8u)

// hi-res counter
#define DIV_LSB (400)

// low-res counter
#define DIV_MSB    ( 62500)
#define MOD_MSB_HI ( 31249)
#define MOD_MSB_LO (-31250)

// combined counters
#define DIV_ALL    ( 25000000)
#define MOD_ALL_HI ( 12499999)
#define MOD_ALL_LO (-12500000)

volatile uint8_t dhcpSec;
volatile uint8_t pllSec;
volatile uint16_t ppsOffset;
volatile uint16_t pllFeedback;

volatile int16_t prevPllError;
volatile uint8_t prevPllUpdate;
volatile uint8_t statsIndex;
volatile int16_t error[RING_SIZE];
volatile uint8_t realigned[RING_SIZE];

uint8_t pllLocked;
float pllError;
float pllErrorRms;


void setPpsOffset(uint16_t offset);

void initGPSDO() {
    ppsOffset = 0;
    pllFeedback = 0;
    statsIndex = 0;
    prevPllUpdate = 0;
    pllLocked = 0;
    pllError = 0;
    pllErrorRms = 0;
    prevPllError = 0;

    // init DAC
    DACB.CTRLC = 0x08u; // AVCC Ref
    DACB.CTRLB = 0x00u; // Enable Channel 0
    DACB.CTRLA = 0x05u; // Enable Channel 0
    while(!(DACB.STATUS & 0x01u)) nop();
    DACB.CH0DATA = 2048u; // start at mid-scale
    pllFeedback = 2048u;

    // PPS Capture
    PORTA.DIRCLR = 0xc0u; // pin 6 + 7
    PORTA.PIN6CTRL = 0x01u; // rising edge
    PORTA.PIN7CTRL = 0x01u; // rising edge
    EVSYS.CH5MUX = 0x56u; // pin PA6
    EVSYS.CH5CTRL = 0x00u;
    EVSYS.CH6MUX = 0x57u; // pin PA7
    EVSYS.CH6CTRL = 0x00u;

    // PPS Prescaling
    TCC1.CTRLB = 0x30u;
    TCC1.CTRLD = 0x2du;
    TCC1.PER = DIV_LSB - 1u;
    // OVF carry
    EVSYS.CH7MUX = 0xc8u;
    EVSYS.CH7CTRL = 0x00u;

    // PPS Generation
    PORTC.DIRSET = 0x03u;
    TCC0.CTRLA = 0x0fu;
    TCC0.CTRLB = 0x33u;
    TCC0.PER = DIV_MSB - 1u;
    setPpsOffset(0);

    // PPS Capture
    TCD0.CTRLA = 0x0fu;
    TCD0.CTRLB = 0x30u;
    TCD0.CTRLD = 0x3du;
    // highest priority capture interrupt
    TCD0.INTCTRLB = 0x03u;
    TCD0.PER = DIV_MSB - 1u;
    TCD0.CCA = 0u;
    TCD0.CCB = 0u;

    // start timers
    TCC0.CNT = 0;
    TCD0.CNT = 0;
    TCC1.CTRLA = 0x01u;
}

uint8_t isPllLocked() {
    return pllLocked;
}

float getPllError() {
    return pllError;
}

float getPllErrorRms() {
    return pllErrorRms;
}

void setPpsOffset(uint16_t offset) {
    if(offset < 56250) {
        TCC0.CCA = offset;
        TCC0.CCB = offset + 6250;
        PORTC.PIN0CTRL = 0x00u;
    } else {
        TCC0.CCA = offset;
        TCC0.CCB = offset - 56249;
        PORTC.PIN0CTRL = 0x40u;
    }
    ppsOffset = offset;
}

int16_t getDelta(uint16_t lsbA, uint16_t msbA, uint16_t lsbB, uint16_t msbB) {
    int32_t a = (((uint32_t)msbA) * DIV_LSB) + lsbA;
    int32_t b = (((uint32_t)msbB) * DIV_LSB) + lsbB;

    // modulo difference
    int32_t diff = a - b;
    if(diff > MOD_ALL_HI)
        diff = DIV_ALL - diff;
    if(diff < MOD_ALL_LO)
        diff = DIV_ALL + diff;

    if(diff > 32767)
        return 32767;
    if(diff < -32768)
        return -32768;
    return (int16_t) diff;
}

inline void decFeedback() {
    if(pllFeedback > 0u) {
        DACB.CH0DATA = --pllFeedback;
    }
}

inline void incFeedback() {
    if(pllFeedback < 4095u) {
        DACB.CH0DATA = ++pllFeedback;
    }
}

inline void alignPPS() {
    int32_t a = (uint32_t) TCD0.CCA;
    int32_t b = (uint32_t) TCD0.CCB;

    // modulo difference
    int32_t diff = a - b;
    if(diff > MOD_MSB_HI)
        diff = DIV_MSB - diff;
    if(diff < MOD_MSB_LO)
        diff = DIV_MSB + diff;

    // force PPS alignment if outside tolerance
    if(diff < 0) diff = -diff;
    if(diff > MAX_PPS_DELTA) {
        setPpsOffset(TCD0.CCA);
        realigned[statsIndex] = 1;
        prevPllError = 0;
    } else {
        realigned[statsIndex] = 0;
    }
}

inline void onRisingPPS() {
    // longer led pulse when locked
    if(pllLocked) ledOn(LED0);

    // check if PPS was realigned
    alignPPS();

    // compute current tracking error
    int16_t currError = getDelta(
            TCC1.CCA, TCD0.CCA,
            TCC1.CCB, TCD0.CCB
    );
    int16_t deltaError = currError - prevPllError;

    // update PLL feedback
    if(PORTB.IN & 1u) {
        if(deltaError <= 0) {
            incFeedback();
        }
    } else {
        if(deltaError >= 0) {
            decFeedback();
        }
    }

    // update status ring
    prevPllError = currError;
    error[statsIndex] = currError;
    statsIndex = (statsIndex + 1u) & (RING_SIZE - 1u);

    // update statistics
    ledOn(LED0);
    pllLocked = 1;
    int32_t acc = 0;
    for(uint8_t i = 0; i < RING_SIZE; i++) {
        acc += error[i];
        pllLocked &= (realigned[i] ^ 1u);
    }
    pllError = acc;
    pllError /= RING_SIZE;
    pllError *= 40e-9f;

    int16_t mean = (int16_t) (acc / RING_SIZE);
    acc = 0;
    for(uint8_t i = 0; i < RING_SIZE; i++) {
        int32_t diff = error[i] - mean;
        acc += diff * diff;
    }
    pllErrorRms = acc;
    pllErrorRms /= RING_SIZE;
    pllErrorRms = sqrtf(pllErrorRms);
    pllErrorRms *= 40e-9f;

    // determine if loop has settled
    if((!pllLocked) || (pllErrorRms > SETTLED_VAR))
        ledOff(LED0);

    // increment pll second counter (exposed for external timekeeping)
    pllSec++;

    // increment dhcp counter
    if(++dhcpSec > 5) {
        dhcpSec = 0;
        dhcp_6sec_tick();
    }
}

// PPS leading edge
ISR(TCD0_CCA_vect, ISR_BLOCK) {
    onRisingPPS();
}
