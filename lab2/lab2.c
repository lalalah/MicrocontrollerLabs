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

// task time_elapsed r  definitions
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

// parameter state name definitions
#define b_freq 0
#define chrp_int 1
#define num_syl 2
#define dur_syl 3
#define rpt_int 4
#define playing 5

// ramp constants
#define RAMPUPEND 250 // = 4*62.5 or 4mSec * 62.5 samples/mSec NOTE:max=255
volatile unsigned int RAMPDOWNSTART;
volatile unsigned int RAMPDOWNEND;
#define countMS 62  //ticks/mSec

// keypad variables
volatile char current_state;    // the current state for the debounce state machine
volatile char button_number;	// the number being pressed on the keypad
volatile char maybe_button;    // the number that might be pressed
volatile char entry_state;    // tracks which part of the parameter entry you are on


// task timers
volatile unsigned char state_timer;
volatile int LED_timer;
volatile unsigned char count_for_ms;

// DDS variables
volatile unsigned int accumulator;
volatile unsigned int increment;
volatile unsigned char highbyte;

volatile signed char sineTable[256];
volatile signed char rampTable[256];
volatile char DDS_en;

// chirp parameters
volatile int burst_frequency;
volatile int chirp_interval;
volatile int num_syllables;
volatile int dur_syllables;
volatile int rpt_interval;

// Time variables
// the volitile is needed because the time is only set in the ISR
// time counts mSec, sample counts DDS samples (62.5 KHz)
volatile unsigned int time_elapsed, time_elapsed_total, sample, rampCount,
					syllableCount, chirpCount;
volatile unsigned char count, LCD_char_count;
volatile char stopped;

const int8_t LCD_initialize[] PROGMEM = "LCD Initialized!\0";
const int8_t LCD_burst_freq[] PROGMEM = "Burst Frequency:\0";
const int8_t LCD_interval[] PROGMEM =  "Chirp Interval: \0";
const int8_t LCD_num_syllable[] PROGMEM = "Num Syllables:  \0";
const int8_t LCD_dur_syllable[] PROGMEM = "Dur Syllables:  \0";
const int8_t LCD_rpt_interval[] PROGMEM = "Rpt interval:   \0";
const int8_t LCD_playing[] PROGMEM = "Chirp, Chirp    \0";
const int8_t LCD_cap_clear[] PROGMEM = "            \0";

volatile int8_t lcd_buffer[17];	// LCD display buffer
volatile int8_t keystr[17];

//key pad scan table
unsigned char key_table[16]={0xd7, 0xee, 0xde, 0xbe,
						  0xed, 0xdd, 0xbd, 0xeb,
						  0xdb, 0xbb, 0x7e, 0x7d,
						  0x7b, 0x77, 0xe7, 0xb7};

// write to LCD
void write_LCD(int num)
begin
	sprintf(lcd_buffer,"%-i", num);
	//sprintf(lcd_buffer + strlen(lcd_buffer), "%c", '.');
	//sprintf(lcd_buffer + strlen(lcd_buffer), "%-i nf  ", capacitance % 10);
	LCDGotoXY(0, 1);
	LCDstring(lcd_buffer, strlen(lcd_buffer));
	//CopyStringtoLCD("test", 1, 1);
end


// Initializes timer0 for fast PWM
void timer0_init(void)
begin
	TCCR0A = 0;
	TIMSK0 = 0;
	TCCR0B = 0;

	// sets to fast_PWM mode (non-inverting) on B.3
	TCCR0A = (1<<COM0A0) | (1<<COM0A1) | (1<<WGM00) | (1<<WGM01);
	TIMSK0 = 1<<TOIE0;
	TCCR0B = 1;    // sets the prescaler to one
	DDRB = (1<<PINB3) ;// make B.3 an output
end


// Allocates a 16-bit mem location for phase loop of DDS
// Creates a sine table in memory to access in DDS
void DDS_init(void)
begin

	accumulator = 0;
	DDS_en = 0;
	increment = 996; 

	// init the sine table
	for (unsigned int i = 0; i < 256; i++)
	begin
		sineTable[i] = (char)(127.0 * sin(6.283*((float)i)/256.0));
		// the following table needs 
		// rampTable[0]=0 and rampTable[255]=127
		rampTable[i] = i>>1 ;
	end

	// init the time counter
   //time=0;
   OCR0A = 128 ; // set PWM to half full scale
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
	LCD_char_count = 0;
end


void update_LCD_state_line(void)
begin
	if (entry_state == b_freq) {CopyStringtoLCD(LCD_burst_freq, 0, 0); CopyStringtoLCD(LCD_cap_clear,0,1);}    // copy LCD_burst_freq to LCD line 0 
	if (entry_state == chrp_int) {CopyStringtoLCD(LCD_interval,0, 0);  CopyStringtoLCD(LCD_cap_clear,0,1);}   // copy LCD_interval to LCD line 0
	if (entry_state == num_syl) {CopyStringtoLCD(LCD_num_syllable, 0, 0);  CopyStringtoLCD(LCD_cap_clear,0,1);}   // copy LCD_num_syllable to LCD line 0
	if (entry_state == dur_syl) {CopyStringtoLCD(LCD_dur_syllable, 0, 0);   CopyStringtoLCD(LCD_cap_clear,0,1);}  // copy LCD_dur_syllable to LCD line 0
	if (entry_state == rpt_int) {CopyStringtoLCD(LCD_rpt_interval, 0, 0);  CopyStringtoLCD(LCD_cap_clear,0,1);}   // copy LCD_rpt_interval to LCD line 0 
	if (entry_state == playing) {CopyStringtoLCD(LCD_playing, 0, 0); CopyStringtoLCD(LCD_cap_clear,0,1);}    // copy LCD_playing to LCD line 0
end


// state machine for parameter entry
void update_entry_state(void)
begin
	entry_state++;
	if(num_syllables == 1 && entry_state == rpt_int) 
	begin
		entry_state++;
		rpt_interval = 0;
	end
	if(entry_state == playing)
	begin
		DDS_en = 1;
		stopped = 0;
	end
	update_LCD_state_line();
	if(entry_state != playing) current_state = released;
end

void initialize(void)
begin

	port_init();
	timer0_init();
	DDS_init();
	LCD_init();
	sei();

	state_timer = t_state;
	current_state = released;
	entry_state = -1;
	LED_timer = t_led;
	count = 0;
	time_elapsed = 0;
	time_elapsed_total = 0;
	stopped = 1;
	update_entry_state();
end


void LED_toggle(void)
begin
	LED_timer = t_led;
	PORTB ^= 0x01;
end

// updates the OCR0A register at 62500 Hz
ISR (TIMER0_OVF_vect)
begin 

	if(DDS_en)
	begin
		//the actual DDS
		accumulator = accumulator + increment ;
		highbyte = (char)(accumulator >> 8) ;
		// output the wavefrom sample
		OCR0A = 128 + (sineTable[highbyte] * rampTable[rampCount]>> 7) ;

		sample++ ;
		if (sample <= RAMPUPEND) rampCount++ ;
		if (sample > RAMPUPEND && sample <= RAMPDOWNSTART ) rampCount = 255 ;
		if (sample > RAMPDOWNSTART && sample <= RAMPDOWNEND ) rampCount-- ;
		if (sample > RAMPDOWNEND) rampCount = 0;
	end

	// generate time base for MAIN
	// 62 counts is about 1 mSec
	count--;
	if (count == 0)
	begin
		if (LED_timer>0)  LED_timer--;
		if (state_timer>0) state_timer--;
		count = countMS;
		time_elapsed++; //in mSec
		time_elapsed_total++;
	end 
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
	_delay_us(5);
	lower = PIND & 0x0f;
	DDRD = 0x0f;
	PORTD = 0xf0;
	_delay_us(5);
	butnum = PIND & 0xf0;
	butnum |= lower;

	i = 20;
	for (i=0;i<17;i++)
	begin
		if (key_table[i] == butnum) return(i);
	end
	return (i);

end


// reads in the value of the string and saves it as a number
int my_str2int(char str[])
begin
	char s2i_count = 0;
	char tens_count = 0;
	int temp = 0;
	int strinteger = 0;

	while(str[s2i_count]!= "/0") s2i_count++;
	while(s2i_count>=0)
	begin
		temp = str[s2i_count];
		if (tens_count) temp = temp*(10*tens_count);
		strinteger += temp;
		s2i_count--;
		tens_count++;
	end

	return strinteger;
end


// saves the recently converted parameter into the relevent global variable
void save_parameter(int data)
begin
	if (entry_state == b_freq)
	begin
		burst_frequency = data;
		increment = (int)(burst_frequency*1.047);
	end
	if (entry_state == chrp_int) chirp_interval = data;
	if (entry_state == num_syl)	num_syllables = data;
	if (entry_state == dur_syl)
	begin
		dur_syllables = data+4;
		RAMPDOWNEND = (dur_syllables)*62.5;
		RAMPDOWNSTART = (dur_syllables-4)*62.5;
	end
	if (entry_state == rpt_int) rpt_interval = data;
end

// displays the current keystr contents on the LCD
void update_LCD(void)
begin
	LCDGotoXY(1,1);
	LCDstring(keystr,strlen(keystr));
end



// state machine for keypad detection
void update_state(void)
begin
	unsigned char nn;
	int parameter_value;
	state_timer = t_state;

	switch(current_state)
	begin
		case done:
		if (entry_state != playing)
			current_state = released;
		break;

		case released:
		if (button_number < 17)
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
		if (button_number == 12)
		begin
			keystr[LCD_char_count ] = '\0';
			current_state = still_term;
			maybe_button = keypad();
		end
		else 
		begin
			if (LCD_char_count <17) keystr[LCD_char_count ++] = button_number + '0';
			update_LCD();
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
			maybe_button = keypad();
		end
		else 
		begin
			current_state = released;
			button_number = keypad();
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
		else 
		begin
			button_number = keypad();
			current_state = done;
			parameter_value = atoi(keystr);
			save_parameter(parameter_value);
			LCD_char_count = 0;
			for (nn = 0; nn<16; nn++) keystr[nn] = '\0';
			update_entry_state();
		end
		break;	
	end

end

void checkStop(void)
begin
	unsigned char clear_count;
	//check stop button
	if (keypad() == 13) 
	begin
		DDS_en = 0;
		entry_state = b_freq;
		update_LCD_state_line();
		current_state = released;
		stopped = 1;
		LCD_char_count = 0;
		for (clear_count = 0; clear_count<16; clear_count++) keystr[clear_count] = '\0';
	end // keypad
end


int main(void)
begin
	initialize();

	while(1)
	begin
		if (!LED_timer) LED_toggle();
		if (!state_timer) update_state();
		checkStop();

	//	if(num_syllables>1)
		begin
			while(!stopped)
			begin
				if (!LED_timer) LED_toggle();

				if (time_elapsed_total >= chirp_interval)
				begin
					time_elapsed_total = 0;
					for (unsigned int j = 0; j < num_syllables; j++)
					begin
						// init ramp variables
						sample = 0 ;
						rampCount = 0;
						// phase lock the sine generator DDS
						accumulator = 0 ;
						// start a new  mSec cycle 
						time_elapsed = 0;

						// after dur_syllables milliSec turn off PWM
			     		DDS_en = 1;
			     		while (time_elapsed < dur_syllables) checkStop();
			     		DDS_en = 0;

						while (time_elapsed < rpt_interval) checkStop();
			     	end // for j
			    end // if time_elapsed
				checkStop();
			end // while DDS_en
		end // if # syll >1
	end //end while
	return 0;
end
