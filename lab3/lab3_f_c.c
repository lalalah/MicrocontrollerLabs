/* Lab 3 - Particle Beam Game
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
#include <avr/sleep.h>

#define begin {
#define end }

// LCD globals
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
volatile char LCD_char_count;

// Fixed Point Maths
#define int2fix(a)   (((int)(a))<<8)            //Convert char to fix. a is a char
#define fix2int(a)   ((signed char)((a)>>8))    //Convert fix to char. a is an int
#define float2fix(a) ((int)((a)*256.0))         //Convert float to fix. a is a float
#define fix2float(a) ((float)(a)/256.0)         //Convert fix to float. a is an int 

// NTSC Timing Constants
#define LINE_TIME 1018 // 20 MHz 1271
#define SLEEP_TIME 999 // 20 MHz 1250

// Screen Constants
#define bytes_per_line 16
#define screen_width (bytes_per_line*8)
#define screen_height 64
#define screen_array_size screen_width*screen_height/8 

#define ScreenTop 45
#define ScreenBot (ScreenTop+screen_height)

//sync
char syncON, syncOFF;


volatile int LineCount;

// Screen buffer and pointer
char screen[screen_array_size];
int* screenindex;

//One bit masks
char pos[8] = {0x80,0x40,0x20,0x10,0x08,0x04,0x02,0x01};

//Ball variables
#define Max_num_balls 12
volatile unsigned int age[Max_num_balls];
volatile signed int x_pos[Max_num_balls];
volatile signed int y_pos[Max_num_balls];
volatile signed int x_velocity[Max_num_balls];
volatile signed int y_velocity[Max_num_balls];
volatile unsigned char is_on_screen[Max_num_balls];
volatile unsigned char hit_count[Max_num_balls];
volatile unsigned char score;
volatile unsigned char time_elapsed_HS;

// put the MCU to sleep JUST before the CompA ISR goes off
ISR(TIMER1_COMPB_vect, ISR_NAKED)
{
	sei();
	sleep_cpu();
	reti();
}


//==================================
//This is the sync generator and raster generator. It MUST be entered from 
//sleep mode to get accurate timing of the sync pulses

ISR (TIMER1_COMPA_vect) {
	int x, screenStart ;
	//start the Horizontal sync pulse    
	PORTD = syncON;

	//update the current scanline number
	LineCount++;   
  
	//begin inverted (Vertical) synch after line 247
	if (LineCount==248) { 
    	syncON = 0b00000001;
    	syncOFF = 0;
  	}
  
	//back to regular sync after line 250
	if (LineCount==251)	{
		syncON = 0;
		syncOFF = 0b00000001;
	}  
  
  	//start new frame after line 262
	if (LineCount==263)
		LineCount = 1;
      
	//adjust to make 5 us pulses
	_delay_us(3);

	//end sync pulse
	PORTD = syncOFF;   

	if (LineCount < ScreenBot && LineCount >= ScreenTop) {
		//compute offset into screen array
		//screenindex = screen + ((LineCount - ScreenTop) << 4) + ((LineCount - ScreenTop) << 3);
		
		//compute offset into screen array
		//screenStart = ((LineCount - ScreenTop) << 4) + ((LineCount - ScreenTop) << 3) ;
		screenStart = (LineCount - ScreenTop) * bytes_per_line;
		//center image on screen
		_delay_us(8);
		//blast the data to the screen
		// We can load UDR twice because it is double-bufffered
		UDR0 = screen[screenStart] ;
		UCSR0B = _BV(TXEN0);
		UDR0 = screen[screenStart+1] ;
		for (x = 2; x < bytes_per_line; x++)
		begin
			while (!(UCSR0A & _BV(UDRE0))) ;
			UDR0 = screen[screenStart+x] ;
		end
		UCSR0B = 0 ;
	}         
}

//init timer 1 to generate sync
void timer1_init(void)
begin
	// Zero previous values
	TCCR1B = 0x00;
	TIMSK1 = 0x00;

	TCCR1B = _BV(WGM12) + _BV(CS10);
	OCR1A = LINE_TIME;	// time for one NTSC line
	OCR1B = SLEEP_TIME;	// time to go to sleep
	TIMSK1 = _BV(OCIE1B) + _BV(OCIE1A);
end

//places USART in MSPIM mode to get 4MHz pixel update
void USART_init(void)
begin
	// USART in MSPIM mode, transmitter enabled, frequency fosc/4
	UCSR0B = _BV(TXEN0);
	UCSR0C = _BV(UMSEL01) | _BV(UMSEL00);
	UBRR0 = 1 ;
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


void ADC_init(void)
begin
	ADMUX = 0;
	ADCSRA = 0;

	ADMUX = (1<<REFS0) + (1<<ADLAR);
	ADCSRA = (1<<ADEN) + 6 ; 
end


void port_init(void)
begin
	DDRA = 0x00;    // all of PORTA is an input to avoid coupling with ADC meas
	PORTA = 0x00;    // no pull-up resistors to avoid coupling

	DDRD = 0x03;    // Sets D.1 and D.0 to output
end


// performs an ADC on the selected channel.
void ADC_start_measure(char channel)
begin
	//ADMUX = 0;
	//ADMUX = (1<<REFS0) + (1<<ADLAR) + channel;
	ADCSRA |= (1<<ADSC);
end


// write to LCD
void write_LCD(char num)
begin
	sprintf(lcd_buffer,"%3d", num);
	LCDGotoXY(0, 1);
	LCDstring(lcd_buffer, strlen(lcd_buffer));
end


void initialize(void)
begin
	ADC_init();
	//LCD_init();
	port_init();
	USART_init();
	timer1_init();

	//initialize synch constants 
	LineCount = 1;

	syncON = 0b00000000;
	syncOFF = 0b00000001;

	// init no balls on screen	
	for(int n=0; n<Max_num_balls;n++)
	begin
		is_on_screen[n] = 0;
		age[n] = 0;
		hit_count[n] = 0;
	end

	for(int y=0; y<screen_array_size;y++) screen[y] = 0;
	time_elapsed_HS = 0;
	score = 0;

	// Set up single video line timing
	sei();
	set_sleep_mode(SLEEP_MODE_IDLE);
	sleep_enable();

end







/////////////////////////////////////////////////////////////////////////////

//===============================================
// Full ascii 5x7 char set
// Designed by: David Perez de la Cruz,and Ed Lau	  
// see: http://instruct1.cit.cornell.edu/courses/ee476/FinalProjects/s2005/dp93/index.html

prog_char ascii[128][7] = {
	//0
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//1
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//2
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//3
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//4
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//5
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//6
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//7
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//8
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//9
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//10
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//11
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//12
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//13
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//14
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//15
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//16
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//17
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//18
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//19
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//20
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//21
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//22
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//23
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//24
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//25
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//26
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//27
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//28
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//29
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//30
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//31
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//32 Space
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//33 Exclamation !
	0b01100000,
	0b01100000,
	0b01100000,
	0b01100000,
	0b00000000,
	0b00000000,
	0b01100000,
	//34 Quotes "
	0b01010000,
	0b01010000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//35 Number #
	0b00000000,
	0b01010000,
	0b11111000,
	0b01010000,
	0b11111000,
	0b01010000,
	0b00000000,
	//36 Dollars $
	0b01110000,
	0b10100000,
	0b10100000,
	0b01110000,
	0b00101000,
	0b00101000,
	0b01110000,
	//37 Percent %
	0b01000000,
	0b10101000,
	0b01010000,
	0b00100000,
	0b01010000,
	0b10101000,
	0b00010000,
	//38 Ampersand &
	0b00100000,
	0b01010000,
	0b10100000,
	0b01000000,
	0b10101000,
	0b10010000,
	0b01101000,
	//39 Single Quote '
	0b01000000,
	0b01000000,
	0b01000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//40 Left Parenthesis (
	0b00010000,
	0b00100000,
	0b01000000,	
	0b01000000,
	0b01000000,
	0b00100000,
	0b00010000,
	//41 Right Parenthesis )
	0b01000000,
	0b00100000,
	0b00010000,
	0b00010000,
	0b00010000,
	0b00100000,
	0b01000000,
	//42 Star *
	0b00010000,
	0b00111000,
	0b00010000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//43 Plus +
	0b00000000,
	0b00100000,
	0b00100000,
	0b11111000,
	0b00100000,
	0b00100000,
	0b00000000,
	//44 Comma ,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00010000,
	0b00010000,
	//45 Minus -
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b11111000,
	0b00000000,
	0b00000000,
	//46 Period .
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00010000,
	// 47 Backslash /
	0b00000000,
	0b00001000,
	0b00010000,
	0b00100000,
	0b01000000,
	0b10000000,
	0b00000000,
	// 48 Zero
	0b01110000,
	0b10001000,
	0b10011000,
	0b10101000,
	0b11001000,
	0b10001000,
	0b01110000,
	//49 One
	0b00100000,
	0b01100000,
	0b00100000,
	0b00100000,
	0b00100000,
	0b00100000,
	0b01110000,  
	//50 two
	0b01110000,
	0b10001000,
	0b00001000,
	0b00010000,
	0b00100000,
	0b01000000,
	0b11111000,
	 //51 Three
	0b11111000,
	0b00010000,
	0b00100000,
	0b00010000,
	0b00001000,
	0b10001000,
	0b01110000,
	//52 Four
	0b00010000,
	0b00110000,
	0b01010000,
	0b10010000,
	0b11111000,
	0b00010000,
	0b00010000,
	//53 Five
	0b11111000,
	0b10000000,
	0b11110000,
	0b00001000,
	0b00001000,
	0b10001000,
	0b01110000,
	//54 Six
	0b01000000,
	0b10000000,
	0b10000000,
	0b11110000,
	0b10001000,
	0b10001000,
	0b01110000,
	//55 Seven
	0b11111000,
	0b00001000,
	0b00010000,
	0b00100000,
	0b01000000,
	0b10000000,
	0b10000000,
	//56 Eight
	0b01110000,
	0b10001000,
	0b10001000,
	0b01110000,
	0b10001000,
	0b10001000,
	0b01110000,
	//57 Nine
	0b01110000,
	0b10001000,
	0b10001000,
	0b01111000,
	0b00001000,
	0b00001000,
	0b00010000,
	//58 :
	0b00000000,
	0b00000000,
	0b00100000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00100000,
	//59 ;
	0b00000000,
	0b00000000,
	0b00100000,
	0b00000000,
	0b00000000,
	0b00100000,
	0b00100000,
	//60 <
	0b00000000,
	0b00011000,
	0b01100000,
	0b10000000,
	0b01100000,
	0b00011000,
	0b00000000,
	//61 =
	0b00000000,
	0b00000000,
	0b01111000,
	0b00000000,
	0b01111000,
	0b00000000,
	0b00000000,
	//62 >
	0b00000000,
	0b11000000,
	0b00110000,
	0b00001000,
	0b00110000,
	0b11000000,
	0b00000000,
	//63 ?
	0b00110000,
	0b01001000,
	0b00010000,
	0b00100000,
	0b00100000,
	0b00000000,
	0b00100000,
	//64 @
	0b01110000,
	0b10001000,
	0b10111000,
	0b10101000,
	0b10010000,
	0b10001000,
	0b01110000,
	//65 A
	0b01110000,
	0b10001000,
	0b10001000,
	0b10001000,
	0b11111000,
	0b10001000,
	0b10001000,
	//B
	0b11110000,
	0b10001000,
	0b10001000,
	0b11110000,
	0b10001000,
	0b10001000,
	0b11110000,
	//C
	0b01110000,
	0b10001000,
	0b10000000,
	0b10000000,
	0b10000000,
	0b10001000,
	0b01110000,
	//D
	0b11110000,
	0b10001000,
	0b10001000,
	0b10001000,
	0b10001000,
	0b10001000,
	0b11110000,
	//E
	0b11111000,
	0b10000000,
	0b10000000,
	0b11111000,
	0b10000000,
	0b10000000,
	0b11111000,
	//F
	0b11111000,
	0b10000000,
	0b10000000,
	0b11111000,
	0b10000000,
	0b10000000,
	0b10000000,
	//G
	0b01110000,
	0b10001000,
	0b10000000,
	0b10011000,
	0b10001000,
	0b10001000,
	0b01110000,
	//H
	0b10001000,
	0b10001000,
	0b10001000,
	0b11111000,
	0b10001000,
	0b10001000,
	0b10001000,
	//I
	0b01110000,
	0b00100000,
	0b00100000,
	0b00100000,
	0b00100000,
	0b00100000,
	0b01110000,
	//J
	0b00111000,
	0b00010000,
	0b00010000,
	0b00010000,
	0b00010000,
	0b10010000,
	0b01100000,
	//K
	0b10001000,
	0b10010000,
	0b10100000,
	0b11000000,
	0b10100000,
	0b10010000,
	0b10001000,
	//L
	0b10000000,
	0b10000000,
	0b10000000,
	0b10000000,
	0b10000000,
	0b10000000,
	0b11111000,
	//M
	0b10001000,
	0b11011000,
	0b10101000,
	0b10101000,
	0b10001000,
	0b10001000,
	0b10001000,
	//N
	0b10001000,
	0b10001000,
	0b11001000,
	0b10101000,
	0b10011000,
	0b10001000,
	0b10001000,
	//O
	0b01110000,
	0b10001000,
	0b10001000,
	0b10001000,
	0b10001000,
	0b10001000,
	0b01110000,
	//P
	0b11110000,
	0b10001000,
	0b10001000,
	0b11110000,
	0b10000000,
	0b10000000,
	0b10000000,
	//Q
	0b01110000,
	0b10001000,
	0b10001000,
	0b10001000,
	0b10101000,
	0b10010000,
	0b01101000,
	//R
	0b11110000,
	0b10001000,
	0b10001000,
	0b11110000,
	0b10100000,
	0b10010000,
	0b10001000,
	//S
	0b01111000,
	0b10000000,
	0b10000000,
	0b01110000,
	0b00001000,
	0b00001000,
	0b11110000,
	//T
	0b11111000,
	0b00100000,
	0b00100000,
	0b00100000,
	0b00100000,
	0b00100000,
	0b00100000,
	//U
	0b10001000,
	0b10001000,
	0b10001000,
	0b10001000,
	0b10001000,
	0b10001000,
	0b01110000,
	//V
	0b10001000,
	0b10001000,
	0b10001000,
	0b10001000,
	0b10001000,
	0b01010000,
	0b00100000,
	//W
	0b10001000,
	0b10001000,
	0b10001000,
	0b10101000,
	0b10101000,
	0b10101000,
	0b01010000,
	//X
	0b10001000,
	0b10001000,
	0b01010000,
	0b00100000,
	0b01010000,
	0b10001000,
	0b10001000,
	//Y
	0b10001000,
	0b10001000,
	0b10001000,
	0b01010000,
	0b00100000,
	0b00100000,
	0b00100000,
	//Z
	0b11111000,
	0b00001000,
	0b00010000,
	0b00100000,
	0b01000000,
	0b10000000,
	0b11111000,
	//91 [
	0b11100000,
	0b10000000,
	0b10000000,
	0b10000000,
	0b10000000,
	0b10000000,
	0b11100000,
	//92 (backslash)
	0b00000000,
	0b10000000,
	0b01000000,
	0b00100000,
	0b00010000,
	0b00001000,
	0b00000000,
	//93 ]
	0b00111000,
	0b00001000,
	0b00001000,
	0b00001000,
	0b00001000,
	0b00001000,
	0b00111000,
	//94 ^
	0b00100000,
	0b01010000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//95 _
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b11111000,
	//96 `
	0b10000000,
	0b01000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	//97 a
	0b00000000,
	0b01100000,
	0b00010000,
	0b01110000,
	0b10010000,
	0b01100000,
	0b00000000,
	//98 b
	0b10000000,
	0b10000000,
	0b11100000,
	0b10010000,
	0b10010000,
	0b11100000,
	0b00000000,
	//99 c
	0b00000000,
	0b00000000,
	0b01110000,
	0b10000000,
	0b10000000,
	0b01110000,
	0b00000000,
	// 100 d
	0b00010000,
	0b00010000,
	0b01110000,
	0b10010000,
	0b10010000,
	0b01110000,
	0b00000000,
	//101 e
	0b00000000,
	0b01100000,
	0b10010000,
	0b11110000,
	0b10000000,
	0b01110000,
	0b00000000,
	//102 f
	0b00110000,
	0b01000000,
	0b11100000,
	0b01000000,
	0b01000000,
	0b01000000,
	0b00000000,
	//103 g
	0b00000000,
	0b01100000,
	0b10010000,
	0b01110000,
	0b00010000,
	0b00010000,
	0b01100000,
	//104 h
	0b10000000,
	0b10000000,
	0b11100000,
	0b10010000,
	0b10010000,
	0b10010000,
	0b00000000,
	//105 i
	0b00000000,
	0b00100000,
	0b00000000,
	0b00100000,
	0b00100000,
	0b00100000,
	0b00000000,
	//106 j
	0b00000000,
	0b00010000,
	0b00000000,
	0b00010000,
	0b00010000,
	0b00010000,
	0b01100000,
	//107 k
	0b10000000,
	0b10010000,
	0b10100000,
	0b11000000,
	0b10100000,
	0b10010000,
	0b00000000,
	//108 l
	0b00100000,
	0b00100000,
	0b00100000,
	0b00100000,
	0b00100000,
	0b00100000,
	0b00000000,
	//109 m
	0b00000000,
	0b00000000,
	0b01010000,
	0b10101000,
	0b10101000,
	0b10101000,
	0b00000000,
	//110 n
	0b00000000,
	0b00000000,
	0b01100000,
	0b10010000,
	0b10010000,
	0b10010000,
	0b00000000,
	//111 o
	0b00000000,
	0b00000000,
	0b01100000,
	0b10010000,
	0b10010000,
	0b01100000,
	0b00000000,
	//112 p
	0b00000000,
	0b00000000,
	0b01100000,
	0b10010000,
	0b11110000,
	0b10000000,
	0b10000000,
	//113 q
	0b00000000,
	0b00000000,
	0b01100000,
	0b10010000,
	0b11110000,
	0b00010000,
	0b00010000,
	//114 r
	0b00000000,
	0b00000000,
	0b10111000,
	0b01000000,
	0b01000000,
	0b01000000,
	0b00000000,
	//115 s
	0b00000000,
	0b00000000,
	0b01110000,
	0b01000000,
	0b00010000,
	0b01110000,
	0b00000000,
	//116 t
	0b01000000,
	0b01000000,
	0b11100000,
	0b01000000,
	0b01000000,
	0b01000000,
	0b00000000,
	// 117u
	0b00000000,
	0b00000000,
	0b10010000,
	0b10010000,
	0b10010000,
	0b01100000,
	0b00000000,
	//118 v
	0b00000000,
	0b00000000,
	0b10001000,
	0b10001000,
	0b01010000,
	0b00100000,
	0b00000000,
	//119 w
	0b00000000,
	0b00000000,
	0b10101000,
	0b10101000,
	0b01010000,
	0b01010000,
	0b00000000,
	//120 x
	0b00000000,
	0b00000000,
	0b10010000,
	0b01100000,
	0b01100000,
	0b10010000,
	0b00000000,
	//121 y
	0b00000000,
	0b00000000,
	0b10010000,
	0b10010000,
	0b01100000,
	0b01000000,
	0b10000000,
	//122 z
	0b00000000,
	0b00000000,
	0b11110000,
	0b00100000,
	0b01000000,
	0b11110000,
	0b00000000,
	//123 {
	0b00100000,
	0b01000000,
	0b01000000,
	0b10000000,
	0b01000000,
	0b01000000,
	0b00100000,
	//124 |
	0b00100000,
	0b00100000,
	0b00100000,
	0b00100000,
	0b00100000,
	0b00100000,
	0b00100000,
	//125 }
	0b00100000,
	0b00010000,
	0b00010000,
	0b00001000,
	0b00010000,	
	0b00010000,
	0b00100000,
	//126 ~
	0b00000000,
	0b00000000,
	0b01000000,
	0b10101000,
	0b00010000,
	0b00000000,
	0b00000000,
	//127 DEL
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000,
	0b00000000
};



//==================================
//plot one point 
//at x,y with color 1=white 0=black 2=invert 
void video_pt(char x, char y, char c) {
	//each line has 18 bytes
	//calculate i based upon this and x,y
	// the byte with the pixel in it
	//int i = (x >> 3) + ((int)y<<4) + ((int)y<<1);
	int i = (x >> 3) + (int)y * bytes_per_line ;

	if (c==1)
	  screen[i] = screen[i] | pos[x & 7];
    else if (c==0)
	  screen[i] = screen[i] & ~pos[x & 7];
    else
	  screen[i] = screen[i] ^ pos[x & 7];
}

//==================================
//plot a line 
//at x1,y1 to x2,y2 with color 1=white 0=black 2=invert 
//NOTE: this function requires signed chars   
//Code is from David Rodgers,
//"Procedural Elements of Computer Graphics",1985
void video_line(char x1, char y1, char x2, char y2, char c) {
	int e;
	signed int dx,dy,j, temp;
	signed char s1,s2, xchange;
    signed int x,y;
        
	x = x1;
	y = y1;
	
	//take absolute value
	if (x2 < x1) {
		dx = x1 - x2;
		s1 = -1;
	}

	else if (x2 == x1) {
		dx = 0;
		s1 = 0;
	}

	else {
		dx = x2 - x1;
		s1 = 1;
	}

	if (y2 < y1) {
		dy = y1 - y2;
		s2 = -1;
	}

	else if (y2 == y1) {
		dy = 0;
		s2 = 0;
	}

	else {
		dy = y2 - y1;
		s2 = 1;
	}

	xchange = 0;   

	if (dy>dx) {
		temp = dx;
		dx = dy;
		dy = temp;
		xchange = 1;
	} 

	e = ((int)dy<<1) - dx;  
	 
	for (j=0; j<=dx; j++) {
		video_pt(x,y,c);
		 
		if (e>=0) {
			if (xchange==1) x = x + s1;
			else y = y + s2;
			e = e - ((int)dx<<1);
		}

		if (xchange==1) y = y + s2;
		else x = x + s1;

		e = e + ((int)dy<<1);
	}
}

//==================================
// put a big character on the screen
// c is index into bitmap
void video_putchar(char x, char y, char c) { 
    char i;
	char y_pos;
	uint8_t j;

	for (i=0;i<7;i++) {
        y_pos = y + i;

		j = pgm_read_byte(((uint32_t)(ascii)) + c*7 + i);

        video_pt(x,   y_pos, (j & 0x80)==0x80);  
        video_pt(x+1, y_pos, (j & 0x40)==0x40); 
        video_pt(x+2, y_pos, (j & 0x20)==0x20);
        video_pt(x+3, y_pos, (j & 0x10)==0x10);
        video_pt(x+4, y_pos, (j & 0x08)==0x08);
    }
}

//==================================
// put a string of big characters on the screen
void video_puts(char x, char y, char *str) {
	char i;
	for (i=0; str[i]!=0; i++) { 
		video_putchar(x,y,str[i]);
		x = x+6;	
	}
}

//return the value of one point 
//at x,y with color 1=white 0=black 2=invert
char video_set(char x, char y) {
	//The following construction 
  	//detects exactly one bit at the x,y location
	// int i = (x>>3) + ((int)y<<4) + ((int)y<<3);
	int i = (x>>3) + (int)y * bytes_per_line ;

    return (screen[i] & 1<<(7-(x & 0x7)));   	
}

//////////////////////////////////////////////////////////////////////////////

void remove_ball(int i)
begin
	video_pt(fix2int(x_pos[i])+1,fix2int(y_pos[i]),0);
	video_pt(fix2int(x_pos[i])+2,fix2int(y_pos[i]),0);
	video_pt(fix2int(x_pos[i]),fix2int(y_pos[i])+1,0);
	video_pt(fix2int(x_pos[i]),fix2int(y_pos[i])+2,0);
	video_pt(fix2int(x_pos[i])+3,fix2int(y_pos[i])+1,0);
	video_pt(fix2int(x_pos[i])+3,fix2int(y_pos[i])+2,0);
	video_pt(fix2int(x_pos[i])+1,fix2int(y_pos[i])+3,0);
	video_pt(fix2int(x_pos[i])+2,fix2int(y_pos[i])+3,0);
end

void place_ball(int i)
begin
	video_pt(fix2int(x_pos[i])+1,fix2int(y_pos[i]),1);
	video_pt(fix2int(x_pos[i])+2,fix2int(y_pos[i]),1);
	video_pt(fix2int(x_pos[i]),fix2int(y_pos[i])+1,1);
	video_pt(fix2int(x_pos[i]),fix2int(y_pos[i])+2,1);
	video_pt(fix2int(x_pos[i])+3,fix2int(y_pos[i])+1,1);
	video_pt(fix2int(x_pos[i])+3,fix2int(y_pos[i])+2,1);
	video_pt(fix2int(x_pos[i])+1,fix2int(y_pos[i])+3,1);
	video_pt(fix2int(x_pos[i])+2,fix2int(y_pos[i])+3,1);
end

char remove_oldest_ball(void)
begin
	unsigned char i = 0;
	unsigned char j = 0;
	unsigned int max = 0;
	for (i=0;i<Max_num_balls;i++)
	begin
		if(age[i]> max)
		begin
			j = i;
			max = age[i];
		end	
	end	
	remove_ball(j);
	is_on_screen[j] = 0;
	score++;
	return j;
end

// adds a ball to the screen
void add_ball(void)
begin
	int temp = 0;
	unsigned char i = 0;
	if(is_on_screen[i])
	begin
		i++;
		while(is_on_screen[i] & i<(Max_num_balls-1)) i++;
		if(i==(Max_num_balls-1))  i = remove_oldest_ball(); 
	end

	age[i] = 0;
	is_on_screen[i] = 1;
	x_pos[i] = int2fix(120);
	y_pos[i] = int2fix(14);
	x_velocity[i] = int2fix(-1); //xe200;
	temp = time_elapsed_HS % 8; 
	y_velocity[i] = int2fix(temp)>>2;//((signed int)time_elapsed_HS)<<2;
	place_ball(i);
end

int main(void)
begin
	signed int rij_x;
	signed int rij_y;
	signed int vij_x;
	signed int vij_y;
	signed int dot_prod;
	char width = screen_width-1;
	char height = screen_height-1;
	unsigned char prev_top = 0;
	unsigned char top_of_paddle;
	int frame_count = 0;
	int v_paddle_y;
	#define v_paddle_x 0
	char max_balls_on_screen = 0;
	char balls_on_screen = 0;
	int delta_x_velocity;
	int delta_y_velocity;
	signed int temp;
	unsigned char time_str[4];
	unsigned char score_str[4];
	unsigned char max_str[4];
	unsigned char real_time_str[4];
	unsigned char adc_str[4];
	//unsigned char start_calc;
	//unsigned char end_calc;

	int test = 0;

	
	initialize();
	
	video_line(width,0,width,height,1);
	video_line(0,0,width,0,1);
	video_line(0,height,width-17,height,1);
	video_pt(60,1,1);
	video_pt(75,1,1);
	video_pt(60,height-1,1);
	video_pt(75,height-1,1);

	while(ADCH<200)
	begin
		video_puts(30,30,"Ready?");
		video_puts(27,(200*53/255)+1,"paddle here");
		// 2. update positions for the paddle
		video_line(5,(200*53/255)+1,20,(200*53/255)+1,1);
		video_pt(6,(200*53/255)+2,1);
		video_pt(6,(200*53/255),1);
		video_line(2,top_of_paddle,2,top_of_paddle+8,0);
		video_line(3,top_of_paddle,3,top_of_paddle+8,0);
		prev_top = top_of_paddle;
		top_of_paddle =(ADCH*53/255)+2;
		v_paddle_y = top_of_paddle-prev_top;
		video_line(2,top_of_paddle,2,top_of_paddle+8,1);
		video_line(3,top_of_paddle,3,top_of_paddle+8,1);
		ADC_start_measure(0);
	end

	video_line(2,top_of_paddle,2,top_of_paddle+8,0);
	video_line(3,top_of_paddle,3,top_of_paddle+8,0);
	video_line(5,(200*53/255)+1,20,(200*53/255)+1,0);
	video_pt(6,(200*53/255)+2,0);
	video_pt(6,(200*53/255),0);
	video_puts(30,30,"       ");
	video_puts(27,(200*53/255)+1,"           ");

	while(time_elapsed_HS<=200)
	begin
		balls_on_screen = 0;
		if (LineCount == ScreenBot)
		begin
			
			// 1. Timing and ball addition
			frame_count++;
			if (frame_count >= 30)
			begin
				add_ball();
				frame_count = 0;
				time_elapsed_HS++; 
				sprintf(time_str, "%3d", (time_elapsed_HS>>1));
				video_puts(110,57,time_str);
				video_line(0,0,width,0,1);
				video_line(0,height,width-17,height,1);
				video_line(width,0,width,height,1);
				video_pt(60,1,1);
				video_pt(75,1,1);
				video_pt(60,height-1,1);
				video_pt(75,height-1,1);
			end

			// 2. update positions for the paddle
			video_line(2,top_of_paddle,2,top_of_paddle+16,0);
			video_line(3,top_of_paddle,3,top_of_paddle+16,0);
			prev_top = top_of_paddle;
			temp =(ADCH*45/255)+1;
			top_of_paddle =(char)temp;
			v_paddle_y = top_of_paddle-prev_top;
			video_line(2,top_of_paddle,2,top_of_paddle+16,1);
			video_line(3,top_of_paddle,3,top_of_paddle+16,1);

			// 3. update ball information
			for(int i = 0; i<Max_num_balls-1;i++)
			begin
				if(!is_on_screen[i]) continue;
				age[i]++;
				balls_on_screen++;
				if (hit_count[i] > 0) hit_count[i]--;

				// 3.1. check for collisions and update velocities (including drag)
				for(int j = i+1; j<Max_num_balls;j++)
				begin
				
					if (i%3 != time_elapsed_HS%3) continue;
					//if(!is_on_screen[j]) continue;
					if(i == j) continue; // don't consider the situation for one ball.
					
					rij_x = x_pos[i] - x_pos[j];
					rij_y = y_pos[i] - y_pos[j];

					if (abs(rij_x) <= 0x0450)
					begin
					 	if (abs(rij_y) <= 0x0450)
					 	begin
							if(hit_count[i]==0 && hit_count[j]==0)// check collision here)<4))
							begin
								vij_x = x_velocity[i] - x_velocity[j];
								vij_y = y_velocity[i] - y_velocity[j];
								//collision code here
								dot_prod = multfix(rij_x,(vij_x>>2)) + multfix(rij_y,(vij_y>>2));
								delta_x_velocity = multfix(rij_x,(dot_prod>>2))>>1;
								delta_y_velocity = multfix(rij_y,(dot_prod>>2))>>1;
								x_velocity[i] += delta_x_velocity;
								y_velocity[i] += delta_y_velocity; 
								x_velocity[j] -= delta_x_velocity;
								y_velocity[j] -= delta_y_velocity; 

								hit_count[i] = 4;
								hit_count[j] = 4;
								
							end // rij check
					 	end
					end
				end // for j

				// drag
				x_velocity[i] -= multfix(x_velocity[i],0x0001);
			    y_velocity[i] -= multfix(y_velocity[i],0x0001);

			
				if((fix2int(x_pos[i]) <= 6)
					&& ((fix2int(y_pos[i])-top_of_paddle) > -5) 
					&& ((fix2int(y_pos[i])-top_of_paddle) < 17))
				begin
					x_velocity[i] = multfix(x_velocity[i],int2fix(-1));
					y_velocity[i] += int2fix(v_paddle_y);
				end
			
		// 3.2. Update position of balls

				remove_ball(i);

				if(fix2int(x_pos[i])>=122)
				begin
					x_velocity[i] = -(x_velocity[i]);
				end
				if(fix2int(y_pos[i])<=3)
				begin
					y_velocity[i] = -(y_velocity[i]);
				end
				if(fix2int(y_pos[i])>=58)
				begin
					y_velocity[i] = -(y_velocity[i]);
				end


				if((x_pos[i] + x_velocity[i])<int2fix(127)) 
				begin
					if((x_pos[i] + x_velocity[i])>int2fix(0)) x_pos[i] += x_velocity[i];
				end

				 //x_pos[i] += x_velocity[i];
				if((y_pos[i] + y_velocity[i])<int2fix(64)) 
				begin
					if((y_pos[i] + y_velocity[i])>int2fix(0)) y_pos[i] += y_velocity[i];
				end


		// 3.3 remove balls that hit the left side of the screen or bins
				if(fix2int(x_pos[i]) <= 2) // hit left wall
				begin
					is_on_screen[i] = 0;
					if(score) score--;
					age[i] = 0;
					if(frame_count<15)add_ball();
					remove_ball(i);
				end // hit left wall
				else
				begin
					if(fix2int(x_pos[i])<75 & fix2int(x_pos[i])>60)
					begin
						if(fix2int(y_pos[i])<=4 | fix2int(y_pos[i])>=(height-5))
						begin
							is_on_screen[i] = 0;
							age[i] = 0;
							score++;
							if(frame_count<=20)add_ball();
							remove_ball(i);
						end // y check bins
						else place_ball(i);
					end // x check bins
					else place_ball(i);
				end // left wall check
			end // for i
			ADC_start_measure(0);
			if(balls_on_screen>max_balls_on_screen) max_balls_on_screen = balls_on_screen;
			// 5. update text (score, time...)
			sprintf(score_str, "%3d",score);
			video_puts(110,1,score_str);
			sprintf(real_time_str, "%3d", balls_on_screen);
			video_puts(6,1,real_time_str);
	
		end // linecount == screenBot
	end // while time < 200

	for(int gh = 0; gh<Max_num_balls; gh++) remove_ball(gh); // clear the screen

	video_line(0,0,width,0,1);
	video_line(0,height,width-17,height,1);
	video_pt(60,1,1);
	video_pt(75,1,1);
	video_pt(60,height-1,1);
	video_pt(75,height-1,1);
	video_line(width,0,width,height,1);
	video_line(1,1,1,height-1,0);
	video_line(2,1,2,height-1,0);

	while(1)
	begin
		sprintf(score_str, "%i",score);
		sprintf(max_str, "%i",max_balls_on_screen);
		video_puts(95,52,max_str);
		video_puts(30,52,"Max balls:");
		video_puts(30,30,"Time Is Up!");
		video_puts(30,42,"Your score:");
		video_puts(100,42,score_str);
	end

	return 1;
end // main
