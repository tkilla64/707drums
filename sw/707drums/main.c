#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/timer.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"

/*
 * TODO:
 *
 */

// Uncomment the line below to measure the time spent inside the ISR
// The STATUS GPIO is used for this; it's an unused GPIO where you can
// hookup an oscilloscope probe and measure the pulsewidth.
//#define ISR_TIME

// Sample ROMs
#include "sample_1a.h"  // BD-1
#include "sample_1b.h"  // BD-2
#include "sample_2a.h"  // SD-1
#include "sample_2b.h"  // SD-2
#include "sample_3.h"   // LOW TOM
#include "sample_4.h"   // MID TOM
#include "sample_5.h"   // HI TOM
#include "sample_6.h"   // HI HAT
#include "sample_7a.h"  // RIM SHOT
#include "sample_7b.h"  // COW BELL
#include "sample_8a.h"  // HAND CLAP
#include "sample_8b.h"  // TAMBOURINE

#define SAMPLE_RATE_US  40      // 25k sample rate, fast timer ISR
#define TICK_1MS        (1000/SAMPLE_RATE_US)   // software timer tick1
#define ENV_ON_US       1000    // On-time for Envelope start pulse
#define BACK_OFF_US     100     // Backoff time in main loop
#define LED_ON_MS       20      // On-time for LEDs
#define SAMPLE_START    0       // start of samples

// Wrap counter determines PWM frequency and resolution
// Resolution: PWM_WRAP steps
// Frequency: 125 MHz / PWM_WRAP
// Value should be n^2 (16, 32, 64, 128, ...)
#define PWM_WRAP        256     // 8-bits resolution
#define PWM_INIT        0x80    // silence DC-offset

// GPIO defines
#define PWM_S1          0   // PWM output BD-1 or BD-2
#define PWM_S2          1   // PWM output SD-1 or SD-2
#define PWM_S3          2   // PWM output LOW TOM
#define PWM_S4          3   // PWM output MID TOM
#define PWM_S5          4   // PWM output HIGH TOM
#define PWM_S6          5   // PWM output HI-HAT
#define PWM_S7          6   // PWM output RIMSHOT or COWBELL
#define PWM_S8          7   // PWM output HANDCLAP or TAMBOURINE
#define TRIG_BD1_N      8   // Trig input BD-1
#define TRIG_BD2_N      9   // Trig input BD-2
#define TRIG_SD1_N      10  // Trig input SD-1
#define TRIG_SD2_N      11  // Trig input SD-2
#define TRIG_LT_N       12  // Trig input LOW TOM
#define TRIG_MT_N       13  // Trig input MID TOM
#define TRIG_HT_N       14  // Trig input HIGH TOM
#define TRIG_RS_N       15  // Trig input RIMSHOT
#define TRIG_COW_N      16  // Trig input COWBELL
#define TRIG_HC_N       17  // Trig input HANDCLAP
#define SDA_1           18  // Reserved pins for EEPROM (not used)
#define SCL_1           19  // Reserved pins for EEPROM (not used)
#define TRIG_TMB_N      20  // Trig input TAMBOURINE
#define TRIG_OHH_N      21  // Trig input OPEN HI-HAT
#define TRIG_CHH_N      22  // Trig input CLOSED HI-HAT
#define LATCH           23  // Serial register Latch output
#define CLOCK           24  // Serial register Clock output 
#define DATA            25  // Serial register Data output
#define HH_CLOSED       26  // HI-HAT Closed output
#define DEBUG_LED       28  // For software debugging
#define STATUS          29  // For software debugging

// Shift registers
#define NO_SR           3   // No of shiftregisters used in this design
#define NO_SR_BITS      (NO_SR*8)
#define MSB_MASK        0x800000

// Shift-register bits
struct shiftreg {
    int bd_env_trig : 1;
    int sd_env_trig : 1;
    int lt_env_trig : 1;
    int mt_env_trig : 1;
    int ht_env_trig : 1;
    int rs_env_trig : 1;
    int hc_env_trig : 1;
    int hh_env_trig : 1;
    int bd1_led     : 1;
    int bd2_led     : 1;
    int sd1_led     : 1;
    int sd2_led     : 1;
    int lt_led      : 1;
    int mt_led      : 1;
    int ht_led      : 1;
    int rim_led     : 1;
    int cow_led     : 1;
    int hcp_led     : 1;
    int tamb_led    : 1;
    int ohh_led     : 1;
    int chh_led     : 1;
    int unused      : 11;   // padding to 32-bits
};

// Global variables for selecting voice and trig playback
volatile bool sample_1_trig    = false;
volatile bool sample_1_alt_snd = false;
volatile bool sample_2_trig    = false;
volatile bool sample_2_alt_snd = false;
volatile bool sample_3_trig    = false;
volatile bool sample_4_trig    = false;
volatile bool sample_5_trig    = false;
volatile bool sample_6_trig    = false;
volatile bool sample_6_running = false;
volatile bool sample_7_trig    = false;
volatile bool sample_7_alt_snd = false;
volatile bool sample_8_trig    = false;
volatile bool sample_8_alt_snd = false;
// Timer tick for driving voice FSM sw timers
uint32_t timercount = 0;
int sw_timer[8] = {0,0,0,0,0,0,0,0};

// Callback function for the repeating timer. 
// This function will be called every 40 microseconds to 
// update the PWM outputs with new sample data. 
// A silent channel with repeat sample[0] (silence).
// Channel 3-5 (Toms) run at half sample rate.
//
// ISR time: 5.02 -> 6.54 us 
bool min_timer_callback(struct repeating_timer *t) {
#ifdef ISR_TIME
    gpio_put(STATUS, 1);  // for measuring time spent in ISR 
#endif

    // Count down sw timers every ms
    if (++timercount % TICK_1MS == 0) {
        for (int i=0 ; i < 8 ; i++) {
            if (sw_timer[i] > 0) {
                sw_timer[i]--;
            }
        }
    }

    // internal variables to keep track of sample playback state
    static bool sample_1_running = false;
    static bool sample_2_running = false;
    static bool sample_3_running = false;
    static bool sample_3_flag =    false;
    static bool sample_4_running = false;
    static bool sample_4_flag =    false;
    static bool sample_5_running = false;
    static bool sample_5_flag =    false;
    static bool sample_7_running = false;
    static bool sample_8_running = false;
    static const unsigned char* sample_1_ptr = &sample_1a_bin[0];
    static const unsigned char* sample_2_ptr = &sample_2a_bin[0];
    static const unsigned char* sample_3_ptr = &sample_3_bin[0];
    static const unsigned char* sample_4_ptr = &sample_4_bin[0];
    static const unsigned char* sample_5_ptr = &sample_5_bin[0];
    static const unsigned char* sample_6_ptr = &sample_6_bin[0];
    static const unsigned char* sample_7_ptr = &sample_7a_bin[0];
    static const unsigned char* sample_8_ptr = &sample_8a_bin[0];

    // Channel 1 BD-1 (1a) or BD-2 (1b). 4k@25k SR
    pwm_set_gpio_level(PWM_S1, *sample_1_ptr);
    if (sample_1_trig) {    // start playback if triggered
        sample_1_ptr = (sample_1_alt_snd ? &sample_1a_bin[SAMPLE_START] : &sample_1b_bin[SAMPLE_START]);
        sample_1_trig = false;
        sample_1_running = true;
    }
    if (sample_1_running) {
        if (sample_1_ptr < (sample_1_alt_snd ? &sample_1a_bin[sample_1a_bin_len] : &sample_1b_bin[sample_1b_bin_len]))
            sample_1_ptr++;
        else {
            sample_1_ptr = (sample_1_alt_snd ? &sample_1a_bin[0] : &sample_1b_bin[0]);
            sample_1_running = false;
        }
    }
    
    // Channel 2 SD-1 (2a) or SD-2 (2b). 4k@25k SR
    pwm_set_gpio_level(PWM_S2, *sample_2_ptr);
    if (sample_2_trig) {    // start playback if triggered
        sample_2_ptr = (sample_2_alt_snd ? &sample_2a_bin[SAMPLE_START] : &sample_2b_bin[SAMPLE_START]);
        sample_2_trig = false;
        sample_2_running = true;
    }
    if (sample_2_running) {
        if (sample_2_ptr < (sample_2_alt_snd ? &sample_2a_bin[sample_2a_bin_len] : &sample_2b_bin[sample_2b_bin_len]))
            sample_2_ptr++;
        else {
            sample_2_ptr = (sample_2_alt_snd ? &sample_2a_bin[0] : &sample_2b_bin[0]);
            sample_2_running = false;
        }
    }

    // Channel 3 LOW TOM. 8k@12.5k SR
    pwm_set_gpio_level(PWM_S3, *sample_3_ptr);
    if (sample_3_trig) {    // start playback if triggered
        sample_3_ptr = &sample_3_bin[SAMPLE_START];
        sample_3_trig = false;
        sample_3_running = true;
    }
    if (sample_3_running) {
        if (sample_3_ptr < &sample_3_bin[sample_3_bin_len])
            if (sample_3_flag) {
                sample_3_ptr++;
                sample_3_flag = false;
            }
            else
                sample_3_flag = true;
        else {
            sample_3_ptr = &sample_3_bin[0];
            sample_3_running = false;
        }
    }

    // Channel 4 MID TOM. 8k@12.5k SR
    pwm_set_gpio_level(PWM_S4, *sample_4_ptr);
    if (sample_4_trig) {    // start playback if triggered
        sample_4_ptr = &sample_4_bin[SAMPLE_START];
        sample_4_trig = false;
        sample_4_running = true;
    }
    if (sample_4_running) {
        if (sample_4_ptr < &sample_4_bin[sample_4_bin_len])
            if (sample_4_flag) {
                sample_4_ptr++;
                sample_4_flag = false;
            }
            else
                sample_4_flag = true;
        else {
            sample_4_ptr = &sample_4_bin[0];
            sample_4_running = false;
        }
    }

    // Channel 5 HIGH TOM. 8k@12.5k SR
    pwm_set_gpio_level(PWM_S5, *sample_5_ptr);
    if (sample_5_trig) {    // start playback if triggered
        sample_5_ptr = &sample_5_bin[SAMPLE_START];
        sample_5_trig = false;
        sample_5_running = true;
    }
    if (sample_5_running) {
        if (sample_5_ptr < &sample_5_bin[sample_5_bin_len])
            if (sample_5_flag) {
                sample_5_ptr++;
                sample_5_flag = false;
            }
            else
                sample_5_flag = true;
        else {
            sample_5_ptr = &sample_5_bin[0];
            sample_5_running = false;
        }
    }

    // Channel 6 HI HAT. 8k@25k SR
    pwm_set_gpio_level(PWM_S6, *sample_6_ptr);
    if (sample_6_trig) {    // start playback if triggered
        sample_6_ptr = &sample_6_bin[SAMPLE_START];
        sample_6_trig = false;
        sample_6_running = true;
    }
    if (sample_6_running) {
        if (sample_6_ptr < &sample_6_bin[sample_6_bin_len])
            sample_6_ptr++;
        else {
            sample_6_ptr = &sample_6_bin[0];
            sample_6_running = false;
        }
    }

    // Channel 7: RIM SHOT (7a) or COW BELL (7b). 4k@25k SR
    pwm_set_gpio_level(PWM_S7, *sample_7_ptr);
    if (sample_7_trig) {    // start playback if triggered
        sample_7_ptr = (sample_7_alt_snd ? &sample_7a_bin[SAMPLE_START] : &sample_7b_bin[SAMPLE_START]);
        sample_7_trig = false;
        sample_7_running = true;
    }
    if (sample_7_running) {
        if (sample_7_ptr < (sample_7_alt_snd ? &sample_7a_bin[sample_7a_bin_len] : &sample_7b_bin[sample_7b_bin_len]))
            sample_7_ptr++;
        else {
            sample_7_ptr = (sample_7_alt_snd ? &sample_7a_bin[0] : &sample_7b_bin[0]);
            sample_7_running = false;
        }
    }

    // Channel 8: HAND CLAP (8a) or TAMBOURINE (8b). 4k@25k SR
    pwm_set_gpio_level(PWM_S8, *sample_8_ptr);
    if (sample_8_trig) {    // start playback if triggered
        sample_8_ptr = (sample_8_alt_snd ? &sample_8a_bin[SAMPLE_START] : &sample_8b_bin[SAMPLE_START]);
        sample_8_trig = false;
        sample_8_running = true;
    }
    if (sample_8_running) {
        if (sample_8_ptr < (sample_8_alt_snd ? &sample_8a_bin[sample_8a_bin_len] : &sample_8b_bin[sample_8b_bin_len]))
            sample_8_ptr++;
        else {
            sample_8_ptr = (sample_8_alt_snd ? &sample_8a_bin[0] : &sample_8b_bin[0]);
            sample_8_running = false;
        }
    }

#ifdef ISR_TIME
    gpio_put(STATUS, 0);  // for measuring time spent in ISR 
#endif    
    return true;
}

// Voice FSM: one for each voice channel. 
// Manages trigger, selection of sound envelope generator and LEDs for each channel.
enum voice_state_t {
    idle,
    trig_a_detect,
    trig_b_detect,
    start,
    wait_for_led
};
int voice_fsm[8] = { idle, idle, idle, idle, idle, idle, idle, idle };
// GPIO edge triggered interrupt callback.
// Note that the trig GPIO inputs are inverted by HW design
void on_edge_event(uint gpio, uint32_t events) 
{
    if (events & GPIO_IRQ_EDGE_FALL) {
        switch (gpio) {
            case TRIG_BD1_N:    // Trig input BD-1
                voice_fsm[0] = trig_a_detect;
                break;
            case TRIG_BD2_N:    // Trig input BD-2
                voice_fsm[0] = trig_b_detect;
            break;
            case TRIG_SD1_N:    // Trig input SD-1
                voice_fsm[1] = trig_a_detect;
                break;
            case TRIG_SD2_N:    // Trig input SD-2
                voice_fsm[1] = trig_b_detect;
                break;
            case TRIG_LT_N:     // Trig input LOW TOM
                voice_fsm[2] = trig_a_detect;
                break;
            case TRIG_MT_N:     // Trig input MID TOM
                voice_fsm[3] = trig_a_detect;
                break;
            case TRIG_HT_N:     // Trig input HIGH TOM
                voice_fsm[4] = trig_a_detect;
                break;
            case TRIG_RS_N:     // Trig input RIMSHOT
                voice_fsm[6] = trig_a_detect;
                break;
            case TRIG_COW_N:    // Trig input COWBELL
                voice_fsm[6] = trig_b_detect;
                break;
            case TRIG_HC_N:     // Trig input HANDCLAP
                voice_fsm[7] = trig_a_detect;
                break;
            case TRIG_TMB_N:    // Trig input TAMBOURINE
                voice_fsm[7] = trig_b_detect;
                break;
            case TRIG_OHH_N:    // Trig input OPEN HI-HAT
                voice_fsm[5] = trig_a_detect;
                break;
            case TRIG_CHH_N:    // Trig input CLOSED HI-HAT
                voice_fsm[5] = trig_b_detect;
                break;
            default:
                break;
        }
    }
}

/*
 * Shift out data to the serial register
 */
void serial_out(uint32_t *data)
{
    volatile bool databit;
    volatile uint32_t shiftdata = *data;

    // Shift out data into the shiftregister (MSB first)
    for (int i = 0 ; i < NO_SR_BITS ; i++) {
        gpio_put(CLOCK, false);
        databit = (((shiftdata << i) & MSB_MASK) == 0) ? false : true;
        gpio_put(DATA, databit);
        sleep_us(1);
        gpio_put(CLOCK, true);  
        sleep_us(1);
    }
    sleep_us(1);
    gpio_put(LATCH, true);
    sleep_us(1);
    gpio_put(CLOCK, false);            
    gpio_put(LATCH, false);
}

int main()
{
    /*
     * MAIN INIT
     */

    volatile struct shiftreg serdata;

    stdio_init_all();

    // Setup PWM outputs for the audio sample channels
    for (int i=PWM_S1 ; i < PWM_S8+1 ; i++) {
        uint slice_num = pwm_gpio_to_slice_num(i);
        gpio_set_function(i, GPIO_FUNC_PWM);
        pwm_set_wrap(slice_num, PWM_WRAP-1);
        pwm_set_gpio_level(i, PWM_INIT);
        pwm_set_enabled(slice_num, true);
    }

    // Setup all trigger inputs
    for (int i=TRIG_BD1_N ; i < TRIG_CHH_N+1 ; i++) {
        if (!(i == SDA_1 || i == SCL_1)) {      // skip these pins (not used)
            gpio_init(i);
            gpio_set_dir(i, GPIO_IN);
            if (i == TRIG_BD1_N)    // hook up the callback with the first GPIO
                gpio_set_irq_enabled_with_callback(i, GPIO_IRQ_EDGE_FALL, true, &on_edge_event);
            else
                gpio_set_irq_enabled(i, GPIO_IRQ_EDGE_FALL, true);
        }
    }

    // Setup output to control HH env gen. 
    gpio_init(HH_CLOSED);
    gpio_set_dir(HH_CLOSED, GPIO_OUT);
    gpio_put(HH_CLOSED, false);

    // Serial port to 74HCT595 shift registers
    gpio_init(DATA);
    gpio_set_dir(DATA, GPIO_OUT);
    gpio_init(CLOCK);
    gpio_set_dir(CLOCK, GPIO_OUT);
    gpio_init(LATCH);
    gpio_set_dir(LATCH, GPIO_OUT);
    gpio_init(DEBUG_LED);
    gpio_set_dir(DEBUG_LED, GPIO_OUT);
    gpio_put(DEBUG_LED, false);

    // Setup STATUS pin (if used)
#ifdef ISR_TIME
    gpio_init(STATUS);
    gpio_set_dir(STATUS, GPIO_OUT);
#endif    

    // Setup fast interupt to service sample output
    struct repeating_timer timer;
    int64_t interval_us = -SAMPLE_RATE_US;
    // Start timer
    bool res = add_repeating_timer_us(interval_us, min_timer_callback, NULL, &timer);
    if (!res) {
        printf("Error: Could not start the sample rate timer!\n");
    }

    // Make sure all LEDs are turned off
    serdata.bd1_led = false;
    serdata.bd2_led = false;
    serdata.sd1_led = false;
    serdata.sd2_led = false;
    serdata.lt_led = false;
    serdata.mt_led = false;
    serdata.ht_led = false;
    serdata.rim_led = false;
    serdata.cow_led = false;
    serdata.hcp_led = false;
    serdata.tamb_led = false;
    serdata.ohh_led = false;
    serdata.chh_led = false;

    // POST of LEDs
    // Light up all LEDs starting with BD_LED (8) and ending with CHH_LED (21)
    // and turn them off in that order. 200ms delay between each and a 1000ms
    // delay when all are lit up
    uint32_t led_data = 0;
    for (int i=8 ; i < 21 ; i++) {
        led_data |= 1<<i;
        serial_out(&led_data);
        sleep_ms(100);
    }
    sleep_ms(800);
    for (int i=8 ; i < 21 ; i++) {
        led_data &= ~1<<i;
        serial_out(&led_data);
        sleep_ms(100);
    }

    // Reset any stray triggers before we start the main loop
    for (int i = 0 ; i < 8 ; i++) {
        voice_fsm[i] = idle;
        sw_timer[i] = 0;
    }
    serdata.bd_env_trig = false;
    serdata.sd_env_trig = false;
    serdata.lt_env_trig = false;
    serdata.mt_env_trig = false;
    serdata.ht_env_trig = false;
    serdata.hh_env_trig = false;
    serdata.rs_env_trig = false;
    serdata.hc_env_trig = false;

    printf("*** 707 DRUMS ***\n\n");

    /*
     *  MAIN LOOP
     */
    while (true) {
        
        // Voice FSMs
        // Voice channel 1: BD-1 and BD-2
        switch (voice_fsm[0]) {
            case trig_a_detect:     // select voice BD-1
                sample_1_alt_snd = false;
                serdata.bd1_led = true;
                voice_fsm[0] = start;
                break;
            case trig_b_detect:     // select voice BD-2
                sample_1_alt_snd = true;
                serdata.bd2_led = true;
                voice_fsm[0] = start;
                break;
            case start:             // trigger sample start, light up LED and start timer
                sample_1_trig = true;
                serdata.bd_env_trig = true;
                serial_out((uint32_t *)&serdata);
                sw_timer[0] = LED_ON_MS;
                sleep_us(ENV_ON_US);
                serdata.bd_env_trig = false;                
                serial_out((uint32_t *)&serdata);
                voice_fsm[0] = wait_for_led;
                break;
            case wait_for_led:      // wait until LED time has expired, then turn off LEDs
                if (sw_timer[0] == 0) {
                    serdata.bd1_led = false;
                    serdata.bd2_led = false;
                    serial_out((uint32_t *)&serdata);
                    voice_fsm[0] = idle;
                }
                break;
            case idle:              // wait for a trigger
            default:
                break;
        }
        // Voice channel 2: SD-1 and SD-2
        switch (voice_fsm[1]) {
            case trig_a_detect:     // select voice SD-1
                sample_2_alt_snd = false;
                serdata.sd1_led = true;
                voice_fsm[1] = start;
                break;
            case trig_b_detect:     // select voice SD-2
                sample_2_alt_snd = true;
                serdata.sd2_led = true;
                voice_fsm[1] = start;
                break;
            case start:             // trigger sample start, light up LED and start timer
                sample_2_trig = true;
                serdata.sd_env_trig = true;
                serial_out((uint32_t *)&serdata);
                sw_timer[1] = LED_ON_MS;
                sleep_us(ENV_ON_US);
                serdata.sd_env_trig = false;                
                serial_out((uint32_t *)&serdata);
                voice_fsm[1] = wait_for_led;
                break;
            case wait_for_led:      // wait until LED time has expired, then turn off LEDs
                if (sw_timer[1] == 0) {
                    serdata.sd1_led = false;
                    serdata.sd2_led = false;
                    serial_out((uint32_t *)&serdata);
                    voice_fsm[1] = idle;
                }
                break;
            case idle:              // wait for a trigger
            default:
                break;
        }
        // Voice channel 3: LOW TOM
        switch (voice_fsm[2]) {
            case trig_a_detect:     // trig voice
                serdata.lt_led = true;
                voice_fsm[2] = start;
                break;
            case start:             // trigger sample start, light up LED and start timer
                sample_3_trig = true;
                serdata.lt_env_trig = true;
                serial_out((uint32_t *)&serdata);
                sw_timer[2] = LED_ON_MS;
                sleep_us(ENV_ON_US);
                serdata.lt_env_trig = false;                
                serial_out((uint32_t *)&serdata);
                voice_fsm[2] = wait_for_led;
                break;
            case wait_for_led:      // wait until LED time has expired, then turn off LED
                if (sw_timer[2] == 0) {
                    serdata.lt_led = false;
                    serial_out((uint32_t *)&serdata);
                    voice_fsm[2] = idle;
                }
                break;
            case idle:              // wait for a trigger
            default:
                break;
        }
        // Voice channel 4: MID TOM
        switch (voice_fsm[3]) {
            case trig_a_detect:     // trig voice
                serdata.mt_led = true;
                voice_fsm[3] = start;
                break;
            case start:             // trigger sample start, light up LED and start timer
                sample_4_trig = true;
                serdata.mt_env_trig = true;
                serial_out((uint32_t *)&serdata);
                sw_timer[3] = LED_ON_MS;
                sleep_us(ENV_ON_US);
                serdata.mt_env_trig = false;                
                serial_out((uint32_t *)&serdata);
                voice_fsm[3] = wait_for_led;
                break;
            case wait_for_led:      // wait until LED time has expired, then turn off LED
                if (sw_timer[3] == 0) {
                    serdata.mt_led = false;
                    serial_out((uint32_t *)&serdata);
                    voice_fsm[3] = idle;
                }
                break;
            case idle:              // wait for a trigger
            default:
                break;
        }
        // Voice channel 5: HIGH TOM
        switch (voice_fsm[4]) {
            case trig_a_detect:     // trig voice
                serdata.ht_led = true;
                voice_fsm[4] = start;
                break;
            case start:             // trigger sample start, light up LED and start timer
                sample_5_trig = true;
                serdata.ht_env_trig = true;
                serial_out((uint32_t *)&serdata);
                sw_timer[4] = LED_ON_MS;
                sleep_us(ENV_ON_US);
                serdata.ht_env_trig = false;                
                serial_out((uint32_t *)&serdata);
                voice_fsm[4] = wait_for_led;
                break;
            case wait_for_led:      // wait until LED time has expired, then turn off LED
                if (sw_timer[4] == 0) {
                    serdata.ht_led = false;
                    serial_out((uint32_t *)&serdata);
                    voice_fsm[4] = idle;
                }
                break;
            case idle:              // wait for a trigger
            default:
                break;
        }
        // Voice channel 6: HI-HAT (Open or Closed)
        // A CHH trig when OHH is running will only mute the HH. Other cases will retrigger sound.
        switch (voice_fsm[5]) {
            case trig_a_detect:
                serdata.ohh_led = true;
                sample_6_trig = true;
                serdata.hh_env_trig = true;
                gpio_put(HH_CLOSED, false);
                voice_fsm[5] = start;
                break;
            case trig_b_detect:
                serdata.chh_led = true;
                if (!sample_6_running) {
                    sample_6_trig = true;
                    serdata.hh_env_trig = true;
                }
                gpio_put(HH_CLOSED, true);
                voice_fsm[5] = start;
                break;
            case start:
                serial_out((uint32_t *)&serdata);
                sw_timer[5] = LED_ON_MS;
                sleep_us(ENV_ON_US);
                serdata.hh_env_trig = false;                
                serial_out((uint32_t *)&serdata);
                voice_fsm[5] = wait_for_led;
                break;
            case wait_for_led:      // wait until LED time has expired, then turn off LEDs
                if (sw_timer[5] == 0) {
                    serdata.ohh_led = false;
                    serdata.chh_led = false;
                    serial_out((uint32_t *)&serdata);
                    voice_fsm[5] = idle;
                }
                break;
            case idle:              // wait for a trigger
            default:
                break;
        }
        // Voice channel 7: RIMSHOT or COWBELL
         switch (voice_fsm[6]) {
            case trig_a_detect:     // select voice Rimshot
                sample_7_alt_snd = false;
                serdata.rim_led = true;
                voice_fsm[6] = start;
                break;
            case trig_b_detect:     // select voice Cowbell
                sample_7_alt_snd = true;
                serdata.cow_led = true;
                voice_fsm[6] = start;
                break;
            case start:             // trigger sample start, light up LED and start timer
                sample_7_trig = true;
                serdata.rs_env_trig = true;
                serial_out((uint32_t *)&serdata);
                sw_timer[6] = LED_ON_MS;
                sleep_us(ENV_ON_US);
                serdata.rs_env_trig = false;                
                serial_out((uint32_t *)&serdata);
                voice_fsm[6] = wait_for_led;
                break;
            case wait_for_led:      // wait until LED time has expired, then turn off LEDs
                if (sw_timer[6] == 0) {
                    serdata.rim_led = false;
                    serdata.cow_led = false;
                    serial_out((uint32_t *)&serdata);
                    voice_fsm[6] = idle;
                }
                break;
            case idle:              // wait for a trigger
            default:
                break;
        }
        // Voice channel 8: HANDCLAP or TAMBOURINE
        switch (voice_fsm[7]) {
            case trig_a_detect:     // select voice Handclap
                sample_8_alt_snd = false;
                serdata.hcp_led = true;
                voice_fsm[7] = start;
                break;
            case trig_b_detect:     // select voice Tambourine
                sample_8_alt_snd = true;
                serdata.tamb_led = true;
                voice_fsm[7] = start;
                break;
            case start:             // trigger sample start, light up LED and start timer
                sample_8_trig = true;
                serdata.hc_env_trig = true;
                serial_out((uint32_t *)&serdata);
                sw_timer[7] = LED_ON_MS;
                sleep_us(ENV_ON_US);
                serdata.hc_env_trig = false;                
                serial_out((uint32_t *)&serdata);
                voice_fsm[7] = wait_for_led;
                break;
            case wait_for_led:      // wait until LED time has expired, then turn off LEDs
                if (sw_timer[7] == 0) {
                    serdata.hcp_led = false;
                    serdata.tamb_led = false;
                    serial_out((uint32_t *)&serdata);
                    voice_fsm[7] = idle;
                }
                break;
            case idle:              // wait for a trigger
            default:
                break;
        }
        sleep_us(BACK_OFF_US);      // short back-off time
   }
}