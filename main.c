  #include	<io.h>
#include   <signal.h>

#define		TXD             BIT1    // TXD on P1.1
#define		RXD		BIT2	// RXD on P1.2

#define     	Bit_time    	208		// 38400 Baud, SMCLK=8MHz (1.0 / 38400) / (1.0 / 8000000) = 208.333
#define		Bit_time_5	104		// Time for half a bit.

unsigned char BitCnt;		// Bit count, used when transmitting byte
unsigned int TXByte;		// Value sent over UART when Transmit() is called
unsigned int RXByte;		// Value recieved once hasRecieved is set

char isReceiving;		// Status for when the device is receiving

#define false 0
#define true  1

// temperature stored in deci-degrees C
int           temperature   = 0;
short         target        = 1800;
unsigned char Error         = 0;
unsigned char ticks         = 0;
char          line[20];
int           line_pos      = 0;
char          line_complete = 0;
int           since_run     = 0;
char          running       = 0;

#define HYSTERESIS 100  // 100 = 1 deg C
#define TICKS_PER_SECOND 244
#define COMPRESSOR_DELAY 60

#define FLASH_SEG_C 0x1040

// Function Definitions
void Transmit(void);

#define RELAY_COOL BIT4
#define RELAY_HEAT BIT3

#define DQBIT BIT5
#define DQ1 P1OUT |= DQBIT
#define DQ0 P1OUT &=~ DQBIT
#define DelayNus(x) delay((x) * 8)

// Delay Routine from mspgcc help file
static void __inline__ delay(register unsigned int n)
{
  __asm__ __volatile__ (
  "1: \n"
  " dec %[n] \n"
  " jne 1b \n"
        : [n] "+r"(n));
}

//-----------------------------------
void DS1820Init (void)
{
    P1DIR |= BIT6 | BIT5;
    P1OUT |= BIT6;
    P1OUT &= ~BIT5;

    P1DIR |= DQBIT;
    DQ0;
    DelayNus(140);//500us
    DQ1;
    P1DIR &= ~DQBIT;

    DelayNus(25);//90us

    if( (P1IN & DQBIT) == DQBIT)        //0001 1111b=1f
    {
	Error = 1;    //1
	P1DIR |= DQBIT;
    }
    else
    {
	Error = 0;//
	P1DIR |= DQBIT;
	DQ1;
    }

    DelayNus(140);//500us
}

void Write_18B20(unsigned char n)
{
    unsigned char i;

    for(i = 0; i < 8; i++)
    {
	DQ0;
	DelayNus(4);//13us
	if((n & 0x01) == 0x01) DQ1;
	else DQ0;
	n = n >> 1;
	DelayNus(14);//50us
	DQ1;
    }
}

//------------------------------------
unsigned char Read_18B20(void)
{
    unsigned char i;
    unsigned char temp = 0;

    for(i=0;i<8;i++)
    {
	temp = temp >> 1;
	DQ0;
	_NOP();//1us
	DQ1;
	_NOP();_NOP();//5us
	_NOP();_NOP();_NOP();
	P1DIR &=~ DQBIT;
	if((P1IN & DQBIT) == 0)
	    temp = temp & 0x7F;
	else
	    temp = temp | 0x80;
	DelayNus(11);//40us
	P1DIR |= DQBIT;
	DQ1;
    }
    return temp;
}

//----------------------------------
void DS1820Skip(void)
{
    Write_18B20(0xcc);
}
//----------------------------------
void DS1820Convert (void)
{
    Write_18B20(0x44);
}

//----------------------------------
void DS1820ReadDo (void)
{
    Write_18B20(0xbe);
}
//----------------------------------

void DS1820ReadTemp(void)
{
    unsigned char temp_low,temp_high; 

    DS1820Init();
    DS1820Skip();
    DS1820ReadDo();

    temp_low=Read_18B20(); 
    temp_high=Read_18B20();
#if 1
	Read_18B20(); // TH
	Read_18B20(); // TL
	Read_18B20(); // reserved
	Read_18B20(); // reserved
    unsigned char remainder = Read_18B20(); // remaining count
#endif

    int TReading = (temp_high << 8) + temp_low;
    char SignBit = TReading & 0x8000;  // test most sig bit

    if (SignBit) // negative
    {
	TReading = (TReading ^ 0xffff) + 1; // 2's comp
    }

    int mult = 100;
    temperature = remainder;
    temperature *= -mult;

    temperature += 16 * mult;
    temperature /= 16;

    temperature -= 25;
    temperature += (TReading >> 1) * mult;
}

void SendString(const char * str)
{
    char toSend;
    while ((toSend = *str++))
    {
	TXByte = toSend;
	Transmit();
    }
}

int atoi(char const* str)
{
    int result = 0;
    char c;
    while( (c = *str++) && c >= '0' && c <= '9' ) // for each non-NUL digit
	result = result*10 + (c-'0'); // multiply previous result by 10
    return result;
}

void itoa(int number)
{
    char result[8];
    int i = 0;
    do
    {
	result[i++] = '0' + (number % 10);
    }
    while ((number /= 10) > 0);
    i--;

    while (i >= 0)
    {
	TXByte = result[i];
	Transmit();
	i--;
    }
}
void save_target()
{
  short *Flash_ptrC= (short *)FLASH_SEG_C;  // Initialize Flash segment C ptr 
  FCTL3 = FWKEY;                            // Clear Lock bit 
  FCTL1 = FWKEY + ERASE;                    // Set Erase bit 
  SendString(" FLASH: ");
  *Flash_ptrC = 0;                          // Dummy write to erase Flash seg C
  itoa(FCTL3 & FAIL);
  FCTL1 = FWKEY + WRT;                      // Set WRT bit for write operation 
  *Flash_ptrC = target;  // save the target temp
  itoa(FCTL3 & FAIL);
  FCTL1 = FWKEY;                            // Clear WRT bit 
  FCTL3 = FWKEY + LOCK;                     // Set LOCK bit 
}

void process_line()
{
    if (line_pos > 0 && line[0] == 's')
    {
	target = atoi(line + 2);
	SendString("set to ");   
	itoa(target);
	save_target();
	SendString("\r\n");
    }
}

void compressor(char on)
{
    if (on)
    {
	if (running || since_run > COMPRESSOR_DELAY)
	{
	    P1OUT |= RELAY_COOL;
	    since_run = 0;
	    running   = 1;
	    return;
	}
    }

    running   = 0;
    since_run++; // keep track of time since last compressor run
    P1OUT &= ~RELAY_COOL;
}

void main(void)
{
	WDTCTL = WDTPW + WDTHOLD;		// Stop WDT

	// Setup the watch dog as an interval timer
	WDTCTL = WDT_MDLY_32;         // 
	IE1 |= WDTIE;                 // enable interrupts for watchdog interval
  
	BCSCTL1 = CALBC1_8MHZ;			// Set range
	DCOCTL  = CALDCO_8MHZ;			// SMCLK = DCO = 8MHz  

	FCTL2 = FWKEY + FSSEL1 + FN1;

	P1SEL |= TXD;
	P1DIR |= TXD | RELAY_COOL | RELAY_HEAT;

	P1OUT = 0;

	P1IES |= RXD;				// RXD Hi/lo edge interrupt
	P1IFG &= ~RXD;				// Clear RXD (flag) before enabling interrupt
	P1IE |= RXD;				// Enable RXD interrupt
  
	isReceiving = false;			// Set initial values
  
	__bis_SR_register(GIE);			// interrupts enabled

	target = *((short *)FLASH_SEG_C);
	if (target < 100 || target > 2500)
	    target = 1800;
  
	SendString("Wireless temp controller\r\n");

	while(1)
	{
	    if (line_complete)
	    {
		line[line_pos] = 0;
		process_line();
		line_pos = 0;
		line_complete = 0;
	    }
	    else
	    {
		DS1820ReadTemp();

		char on = temperature > target + (running ? -HYSTERESIS : HYSTERESIS);

		itoa(temperature);
		SendString(",target=");
		itoa(target);
		SendString(",err=");
		itoa(Error);
		SendString(",on=");
		itoa(on);
		SendString(",delay=");
		itoa(on && since_run < COMPRESSOR_DELAY ? COMPRESSOR_DELAY - since_run : 0); 
		SendString("\r\n");

		compressor(!Error && on);

		// kick of a convert for next time
		DS1820Init();
		DS1820Skip();
		DS1820Convert();

	    }
	    __bis_SR_register(CPUOFF + GIE);        
	    // LPM0, the ADC interrupt will wake the processor up. This is so that it does not
	    //	endlessly loop when no value has been Received.
	}
}

// Function Transmits Character from TXByte 
void Transmit()
{ 
	while(isReceiving);			// Wait for RX completion
  	CCTL0 = OUT;				// TXD Idle as Mark
  	TACTL = TASSEL_2 + MC_2;		// SMCLK, continuous mode

  	BitCnt = 0xA;				// Load Bit counter, 8 bits + ST/SP
  	CCR0 = TAR;				// Initialize compare register
  
  	CCR0 += Bit_time;			// Set time till first bit
  	TXByte |= 0x100;			// Add stop bit to TXByte (which is logical 1)
  	TXByte = TXByte << 1;			// Add start bit (which is logical 0)
  
  	CCTL0 =  CCIS0 + OUTMOD0 + CCIE;	// Set signal, intial value, enable interrupts
  	while ( CCTL0 & CCIE );			// Wait for previous TX completion
}

// Port 1 interrupt service routine
interrupt(PORT1_VECTOR) Port_1(void)
{  	
	isReceiving = true;
	
	P1IE &= ~RXD;			// Disable RXD interrupt
	P1IFG &= ~RXD;			// Clear RXD IFG (interrupt flag)
	
  	TACTL = TASSEL_2 + MC_2;	// SMCLK, continuous mode
  	CCR0 = TAR + Bit_time_5;			// Initialize compare register + Set time till first bit
	CCTL0 = OUTMOD1 + CCIE;		// Dissable TX and enable interrupts
	
	RXByte = 0;			// Initialize RXByte
	BitCnt = 0x9;			// Load Bit counter, 8 bits + ST
}

// Timer A0 interrupt service routine
interrupt(TIMERA0_VECTOR)  Timer_A (void)
{
    if(!isReceiving)
    {
	CCR0 += Bit_time;			// Add Offset to CCR0  
	if ( BitCnt == 0)			// If all bits TXed
	{
	    TACTL = TASSEL_2;		// SMCLK, timer off (for power consumption)
	    CCTL0 &= ~ CCIE ;		// Disable interrupt
	}
	else
	{
	    CCTL0 |=  OUTMOD2;		// Set TX bit to 0
	    if (TXByte & 0x01)
		CCTL0 &= ~ OUTMOD2;	// If it should be 1, set it to 1
	    TXByte = TXByte >> 1;
	    BitCnt --;
	}
    }
    else
    {
	CCR0 += Bit_time;				// Add Offset to CCR0  
	if ( BitCnt == 0)
	{
	    TACTL = TASSEL_2;			// SMCLK, timer off (for power consumption)
	    CCTL0 &= ~ CCIE ;			// Disable interrupt
			
	    isReceiving = false;
			
	    P1IFG &= ~RXD;				// clear RXD IFG (interrupt flag)
	    P1IE |= RXD;				// enabled RXD interrupt
		
//	    if ( (RXByte & 0x201) == 0x200)		// Validate the start and stop bits are correct
	    {
		RXByte = RXByte >> 2;		// Remove start bit
		RXByte &= 0xFF;			// Remove stop bit

		if (RXByte == '\n' || RXByte == '\r')
		{
		    line_complete = 1;
		    __bic_SR_register_on_exit(CPUOFF);	// Enable CPU so the main while loop continues
		}
		else if (!line_complete && line_pos < sizeof(line))
		{
		    line[line_pos++] = RXByte;
		    TXByte = RXByte;	// Load the recieved byte into the byte to be transmitted
//		    Transmit();
		}
	    }
	}
	else
	{
	    if ( (P1IN & RXD) == RXD)		// If bit is set?
		RXByte |= 0x400;		// Set the value in the RXByte 
	    RXByte = RXByte >> 1;			// Shift the bits down
	    BitCnt --;
	}
    }
}

interrupt(WDT_VECTOR) wdt_interrupt (void)
{
    ticks++;

    // wake the mainloop every second
    if (ticks > TICKS_PER_SECOND)
    {
	ticks = 0;
	__bic_SR_register_on_exit(CPUOFF);	// Enable CPU so the main while loop continues
    }
}
