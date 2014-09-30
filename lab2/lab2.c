/* Lab 2 - Cricket Call Generator
	Connor Archard - cwa37
	Feiran Chen - fc254

*/

#define F_CPU 16000000UL
#include "lcd_lib.h"
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <util/delay.h> // needed for lcd_lib
#define begin {
#define end }
typedef enum { false, true } bool;

// task timer definitions
#define t_state 40
#define t_led 500
#define t_ramp 4

// state name definitions
#define done 0
#define released 1
#define maybe_pressed 2
#define detect_term 3
#define pressed 4
#define maybe_released 5
#define still_term 6
#define maybe_term_released 7
#define str_buff_check 8

// ramp constants
#define RAMPUPEND 250 // = 4*62.5 or 4mSec * 62.5 samples/mSec NOTE:max=255
#define RAMPDOWNSTART 625 // = 10*62.5
#define RAMPDOWNEND 875 // = 14*62.5 NOTE: RAMPDOWNEND-RAMPDOWNSTART<255 
#define countMS 62  //ticks/mSec

// keypad variables
volatile char current_state;    // the current state for the debounce state machine
volatile char button_number;	// the number being pressed on the keypad
volatile char maybe_button;    // the number that might be pressed

// task timers
volatile char state_timer;
volatile int LED_timer;
volatile char count_for_ms;

// DDS variables
volatile unsigned long accumulator;
volatile unsigned long increment;
volatile unsigned char highbyte;
volatile char sineTable[256];
volatile char rampTable[256];
volatile char keystr[17];

// Time variables
// the volitile is needed because the time is only set in the ISR
// time counts mSec, sample counts DDS samples (62.5 KHz)
volatile unsigned int time, sample, rampCount;
volatile char  count;

const int8_t LCD_initialize[] PROGMEM = "LCD Initialized!\0";
const int8_t LCD_interval[] PROGMEM =  "Chirp Interval: \0";
const int8_t LCD_num_syllable[] PROGMEM = "Num Syllables:  \0";
const int8_t LCD_dur_syllable[] PROGMEM = "Dur Syllables:  \0";
const int8_t LCD_rpt_interval[] PROGMEM = "Rpt interval:   \0";
const char keytable[16] = {0x7d,0xee,0xed,0xeb,0xde,0xdd,0xdb,0xbe,0xbd,0xbb,0x7e,0x7b,0xe7,0xd7,0xb7,0x77};
const int8_t LCD_cap_clear[] PROGMEM = "            \0";
int8_t lcd_buffer[13];	// LCD display buffer

// ====================== Debug Helper ==========================
// write to LCD
void write_LCD(int num)
begin
	sprintf(lcd_buffer,"%-i", num);
	//sprintf(lcd_buffer + strlen(lcd_buffer), "%c", '.');
	//sprintf(lcd_buffer + strlen(lcd_buffer), "%-i nf  ", capacitance % 10);
	LCDGotoXY(0, 0);
	LCDstring(lcd_buffer, strlen(lcd_buffer));
end

// ==================== End of Debug Helper =====================



// Initializes timer0 for fast PWM
void timer0_init(void)
begin
	TCCR0A = 0;
	TIMSK0 = 0;
	TCCR0B = 0;
	TCCR0A = (1<<COM0A0) + (1<<COM0A1) + (1<<WGM01) + (1<<WGM00);    // sets to fast_PWM mode (non-inverting) on B.3
	TIMSK0 = 1<<TOIE0;
	TCCR0B = 0x01;    // sets the prescaler to one
end


// Allocates a 16-bit mem location for phase loop of DDS
// Creates a sine table in memory to access in DDS
void DDS_init(void)
begin

	accumulator = 0;
    // init the DDS phase increment
	// for a 32-bit DDS accumulator, running at 16e6/256 Hz:
	// increment = 2^32*256*Fout/16e6 = 68719 * Fout
	// Fout=1000 Hz, increment= 68719000 
	increment =68719000L; //68719000L ; 
   // init the sine table
   for (int8_t i = 0; i < 256; i++)
   begin
   		sineTable[i] = (char)(127.0 * sin(6.283*((float)i)/256.0));
		// the following table needs 
		// rampTable[0]=0 and rampTable[255]=127
		rampTable[i] = i>>1 ;
   end  




end

// PORTA - unused
// PORTB - speaker (B.3 is the OC0A that toggles on fast PWM)
// PORTC - LCD
// PORTD - keypad
void port_init(void)
begin
	DDRA = 0xff;    // PORTA is unused and left output low to save power
	DDRB = 0xff;    // PORTB is left as an output to drive the speaker with B.3
	DDRC = 0xff;    // PORTC is used for the LCD and needs to output
	DDRD = 0xf0;    // PORTD is used for keypad reading and is set to half and half to start

	PORTA = 0x00;    // output low
	PORTB = 0x00;    // output low
	PORTC = 0x00;    // output low
	PORTD = 0x0f;    // output low with pull up resistors
end

void LCD_init(void)
begin
	// start the LCD 
	LCDinit();	//initialize the display
	LCDcursorOFF();
	LCDclr();				//clear the display
	LCDGotoXY(0,0);
	CopyStringtoLCD(LCD_initialize, 0, 0);
end


void initialize(void)
begin
	current_state = done;
	state_timer = t_state;
	LED_timer = t_led;
	count = 0;

	port_init();
	LCD_init();
	timer0_init();
	DDS_init();

	sei();
end


// checks keypad and returns key
// returns 255 if invalid keystroke
// if n~=0, will place into released state
char keypad(void)
begin
	char butnum = 0;
	char lower = 0;
	char i;

	DDRD = 0xf0;
	PORTD = 0x0f;
	lower = PIND & 0x0f;
	DDRD = 0x0f;
	PORTD = 0xf0;
	butnum = PIND & 0xf0;
	butnum |= lower;
	i = 20;
	for (i=0;i<17;i++)
	begin
		if (key_table(i) == butnum) return(i);
	end

	return (i-1);

end

// state machine for keypad detection
void update_state(void)
begin
	switch(current_state)
	begin
		case done:

			break;

		case released:
			if (button_number <= 16)
			begin
				current_state = maybe_pressed;
				maybe_button = keypad();
			end
			else button_number = keypad();
			break;

		case maybe_pressed:
			if (button_number == maybe_button)	current_state = detect_term;			
			else 
			begin
				current_state = released;
				button_number = keypad();
			end
			break;

		case detect_term:
			if (button_number == 15)
			begin
			 	keystr[count] = '\0';
			 	current_state = still_term;
			end
			else 
			begin
				if (count<17) keystr[count++] = button_number;
				current_state = pressed;
				maybe_button = keypad();
			end
			break;

		case pressed:
			if (maybe_button == button_number) maybe_button = keypad();
			else
			begin
				current_state = maybe_released;
				maybe_button = keypad();
			end
			break;

		case maybe_released:
			if (maybe_button == button_number)
			begin
				current_state = pressed;
				maybe_ button = keypad();
			end
			else 
			begin
				current_state = released;
				button_number = 20;
			end
			break;

		case still_term:
			if (button_number == maybe_button) maybe_button = keypad();
			else 
			begin
				current_state = maybe_term_released;
				maybe_button = keypad();
			end
			break;

		case maybe_term_released:
			if (button_number == maybe_button) 
			begin
				current_state = still_term;
				maybe_button = keypad();
			end
			else current_state = done;
			break;
	end

end


void LED_toggle(void)
begin
	LED_timer = t_led;
	PORTB ^= 0x01;
end

// updates the OCR0A register at 62500 Hz
ISR(TIMER0_OVF_vect)
begin

	//the actual DDR 
	accumulator = accumulator + increment ;
	highbyte = (char)(accumulator >> 24) ;
	
	// output the wavefrom sample
	OCR0A = 128 + ((sineTable[highbyte] * rampTable[rampCount])>>7) ;
	
	sample++ ;
	if (sample <= RAMPUPEND) rampCount++ ;
	if (sample > RAMPUPEND && sample <= RAMPDOWNSTART ) rampCount = 255 ;
	if (sample > RAMPDOWNSTART && sample <= RAMPDOWNEND ) rampCount-- ;
	if (sample > RAMPDOWNEND) rampCount = 0; 
	
	// generate time base for MAIN
	// 62 counts is about 1 mSec
	count--;
	if (0 == count )
	begin
		count=countMS;
		time++;    //in mSec
		if (state_timer>0) state_timer--;
		if (LED_timer > 0) LED_timer--;
	end  
end


int main(void)
begin

	while(1)
	begin
		//if(state_timer == 0) update_state();
		if(LED_timer == 0) LED_toggle();

		// check s_table here
		// modulate s_table value by ramp
		// update ORC0A through s_table value here
		// timer0 takes care of the rest...
	end

	return 1;
end
