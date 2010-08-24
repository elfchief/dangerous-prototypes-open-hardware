/*
*
*	USB infrared remote control receiver transmitter firmware v1.0
*	License: creative commons - attribution, share-alike 
*	Copyright Ian Lesnet 2010
*	http://dangerousprototypes.com
*
*/
//
//	IR sampler
//
// This new mode counts the duration of each IR pulse/space
// the signal is returned as 16bit timer counts that each represent 21.3us
// the values are returned Pulse-h P-l Blank-h B-l P-h P-l etc.
// the last value will be 0xff 0xff, representing 1.7seconds of blank


//This is a hacked copy of the IRIO source
// it still contains a lot of variables and cruft that's not needed



//#include "globals.h" // by 7


//USB stack
#include "usb_config.h" //download these files from Microchip
#include "./USB/usb.h"
#include "./USB/usb_device.h" 
#include "./USB/usb_function_cdc.h"


extern struct _irtoy irToy;

static unsigned char h,l;

static struct{
	unsigned char T1offsetH;
	unsigned char T1offsetL;
	unsigned char RXsamples;
	unsigned char TXsamples;
	unsigned char timeout;
	unsigned char btrack; //bit tracker
	unsigned char btrackreset; //reset value for bit tracker
	unsigned char work;
	unsigned char rxbuf;
	unsigned char txbuf;
	unsigned char TX:1;
	unsigned char rxflag:1;
	unsigned char txflag:1;
	unsigned char flushflag:1;
	unsigned char overflow:1;
} irIO;

//static unsigned char USBbuf[2];

void irssetup(void){

	//send version string
  	if( mUSBUSARTIsTxTrfReady() ){ //it's always ready, but this could be done better
		irToy.usbOut[0]='S';//answer OK
		irToy.usbOut[1]='0';
		irToy.usbOut[2]='1';
		putUSBUSART(irToy.usbOut,3);
	}

	//setup for IR RX
	irIO.rxflag=0;
	irIO.txflag=0;
	irIO.flushflag=0;
	irIO.timeout=0;
	irIO.RXsamples=0;
	irIO.TXsamples=0;
	irIO.TX=0;
	irIO.overflow=0;

	//setup timer 0
	T0CON=0;
	//configure prescaler
	//bit 2-0 T0PS2:T0PS0: Timer0 Prescaler Select bits
	//111 = 1:256 Prescale value
	//110 = 1:128 Prescale value
	//101 = 1:64 Prescale value
	//100 = 1:32 Prescale value
	//011 = 1:16 Prescale value
	//010 = 1:8 Prescale value
	//001 = 1:4 Prescale value
	//000 = 1:2 Prescale value
	T0CON=0b111;
	//T0CONbits.T08BIT=1; //16bit mode
	//internal clock
	//low to high
	T0CONbits.PSA=0; //1=not assigned

	//timer 1 as USB packet send timeout
	T1CON=0;

	IRRX_IE = 1;  //IR RX interrupt on
	IRRX_IF = 0;  //IR RX interrupt on
}


//
// irIO periodic service routine
// moves bytes in and out
//
unsigned char irsservice(void){
	static unsigned char txBufferCount;

	static enum _smio { //in out data state machine
		I_IDLE = 0,
		I_PARAMETERS,
		I_PROCESS,
	} irIOstate = I_IDLE;

	static struct _smCommand {
		unsigned char command[5];
		unsigned char parameters;
		unsigned char parCnt;
	} irIOcommand;

	if(irIO.TXsamples==0){
		irIO.TXsamples=getsUSBUSART(irToy.s,64);
		txBufferCount=0;
	}

	if(irIO.TXsamples>0){
		switch (irIOstate){
			case I_IDLE:
					#define IRIO_RESET 0x00
					#define IRIO_SETUP_SAMPLETIMER 0x01
					#define IRIO_SETUP_PWM 0x02
					#define IRIO_RAW 0x03
					#define IRIO_REPLAY 0x04			
					switch(irToy.s[txBufferCount]){
						case IRIO_RESET: //reset, return to RC5 (same as SUMP) 
							CCP1CON=0; 
							TM0ON=0;
							TM0IE=0;
							T0CON=0;
							T2ON=0; 
							T1ON=0; 
							T1IE=0;
							IRRX_IE = 0;
							IRTX_TRIS|=IRTX_PIN;//digital INPUT (no PWM until active)
							IRTX_LAT&=(~IRTX_PIN);//direction 0
							LEDoff();
							LEDout();
							return 1; //need to flag exit!
							break;
						case IRIO_SETUP_PWM: //setup PWM frequency
							irIOcommand.command[0]=irToy.s[txBufferCount];
							irIOcommand.parameters=2;
							irIOstate=I_PARAMETERS;
							break;
						default:
							break;
					}
					irIO.TXsamples--;
					txBufferCount++;
				break;
			case I_PARAMETERS://get optional parameters
				irIOcommand.command[irIOcommand.parCnt]=irToy.s[txBufferCount];//store each parameter
				irIO.TXsamples--;
				txBufferCount++;
				irIOcommand.parCnt++;
				if(irIOcommand.parCnt<irIOcommand.parameters) break; //if not all parameters, quit
			case I_PROCESS:	//process long commands
				switch(irIOcommand.command[0]){
					case IRIO_SETUP_PWM: //setup user defined PWM frequency
						T2CON = 0;
						PR2 = irIOcommand.command[1];//user period
						CCPR1L =(irIOcommand.command[1]>>1);//upper 8 bits of duty cycle, 50% of period by binary division
						if((irIOcommand.command[1]& 0b1)!=0)//if LSB is set, set bit 1 in CCP1CON
							CCP1CON = 0b00101100 ; //5-4 two LSB of duty, 3-0 set PWM
						else
							CCP1CON = 0b00001100 ; //5-4 two LSB of duty, 3-0 set PWM

						T2CON = 0b00000101; //enable timer again, 4x prescaler				
						break;
				}
				irIOstate=I_IDLE;//return to idle state
				break;	

		}//switch 
	}

	if(irIO.overflow==0){
		//service the inbound samples here
		//keep in 64 byte buffer then send to USB for max sample rate
		if(irIO.rxflag==1){ //a RX byte is in the buffer
			if(irIO.RXsamples<=62){ //if we have room in the USB send buffer
				irToy.usbOut[irIO.RXsamples]=h; //add to USB send buffer
				irIO.RXsamples++;
				irToy.usbOut[irIO.RXsamples]=l; //add to USB send buffer
				irIO.RXsamples++;
				irIO.rxflag=0;				//reset the flag
			//removed this check. we might hit this several times while before the send can clear
			//that can be perfectly fine, as long as it happens before we need to move the next samples in
			//the new overflow check in interrupt routine will also catch this error
			//}else{//underrun error, no more room!
			//	irIO.overflow=1;
			//	LED_TRIS |= LED_PIN; //error, LED off by making it input 
			}
		}

		//if the buffer is full, send it to USB
		if( ( (irIO.RXsamples==64) || (irIO.flushflag==1) ) && (mUSBUSARTIsTxTrfReady()) ){ //if we have full buffer, or end of capture flush
			putUSBUSART(irToy.usbOut,irIO.RXsamples);//send current buffer to USB
			irIO.RXsamples=0;
			irIO.flushflag=0;
		}
	}else{//overflow error
		//on overflow we loop until we can send 6 0xff then reset everything
		if(mUSBUSARTIsTxTrfReady()){
			irToy.usbOut[0]=0xff; //add to USB send buffer
			irToy.usbOut[1]=0xff; //add to USB send buffer
			irToy.usbOut[2]=0xff; //add to USB send buffer
			irToy.usbOut[3]=0xff; //add to USB send buffer
			irToy.usbOut[4]=0xff; //add to USB send buffer
			irToy.usbOut[5]=0xff; //add to USB send buffer
			irIO.RXsamples=6;
			putUSBUSART(irToy.usbOut,irIO.RXsamples);//send current buffer to USB

			T1ON=0;		//t1 is the usb packet timeout, disable it
			TM0ON=0; //timer0 off
			TM0IF=0;
			//reset the pin interrupt, just in case
			IRRX_IE=1;
			IRRX_IF=0;

			irIO.RXsamples=0;
			irIO.flushflag=0;
			irIO.overflow=0;

			LED_LAT &=(~LED_PIN); //LED off
		}
	}


	
	return 0;//CONTINUE
}

//the first falling edge starts timer0
//the next pin interrupt copies the timer0 value to a buffer and resets timer0
//if timer0 interrupts, then timeout and end.
//high priority interrupt routine
#pragma interrupt irsInterruptHandlerHigh
void irsInterruptHandlerHigh (void){

	//Increment ticks with prescaler included 21.33333e-06   Prescaler = 256 16-bit timer
	if(IRRX_IE==1 && IRRX_IF == 1){ //if RB Port Change Interrupt	
		l=IRRX_PORT;
		if(TM0ON==0){ //timer not running, setup and start

			if( ((l & IRRX_PIN)==0)){;//only if 0, must read PORTB to clear RBIF
				LED_LAT |= LED_PIN;//LED ON
				TMR0H=0;//first set the high byte
				TMR0L=0;//set low byte copies high byte too
				TM0IE=1;
				TM0IF=0;
				TM0ON=1;//enable the timer
				
				TMR1H=0;
				TMR1L=0;
				T1IF=0;		//clear the interrupt flag
				T1IE=1; 	//able interrupts...
				T1ON=1;		//timer on
			}

		}else{//timer running, save value and reset
			//the goal is to reset the timer as quickly as possible
			//later we can fine tune the start value to compensate for the lost cycles
			TM0ON=0;//disable the timer
			l=TMR0L;//read low byte, puts high byte in H
			h=TMR0H; //read high byte
			TMR0H=0;//first set the high byte
			TMR0L=0;//set low byte copies high byte too
			TM0IF=0;
			TM0ON=1;//enable the timer

			//reset timer1, USB packet send timeout
			T1ON=0;		//timer on
			TMR1H=0;
			TMR1L=0;
			T1ON=1;		//timer on
			
			if(irIO.rxflag==0){//check if data is pending
				irIO.rxflag=1;
			}else{//error, overflow
				irIO.overflow=1;
			}
			
		}
		//clear portb interrupt		
    	IRRX_IF=0;    //Reset the RB Port Change Interrupt Flag bit  

	}else if(TM0IE==1 && TM0IF==1){ //is this timer 0 interrupt?
		//the idea is that if we got here
		//it has been so long without a pin change that 
		//there is not more signal
		//it would be more robust to check the pin state for 0
		//need to examine the limits of typical protocols closer

		T1ON=0;		//t1 is the usb packet timeout, disable it

		TM0ON=0; //timer0 off
		TM0IF=0;

		if(irIO.rxflag==0){//check if data is pending
			//packet terminator, 1.7S with no signal
			h=0xff; //add to USB send buffer
			l=0xff; //add to USB send buffer
			irIO.rxflag=1;
			//set the flush flag to send the packet from the main loop
			irIO.flushflag=1;
		}else{//error, overflow
			irIO.overflow=1;
		}

		//reset the pin interrupt, just in case
		IRRX_IE=1;
		IRRX_IF=0;

		LED_LAT &=(~LED_PIN); //LED off
	}else if(T1IE==1 && T1IF==1){ //is this timer 1 interrupt?
		//this is another timer
		//it tells the main loop to send any pending USB bytes
		// after a few MS
		//the idea is that the 1.7s delay for the terminaor byte is really long
		//we want to send the accumulated data sooner than that, or response will appear sluggish
		//time1 (adjust as needed) sets teh flush flag and sends any pending data on it's way
		irIO.flushflag=1;	
		T1IF=0;		//clear the interrupt flag
	}  
}










/////////////// 7's scratch //////////////////////

static u8 IRs_TransmitInversion;

#define IRS_TRANSMIT_HI	0
#define IRS_TRANSMIT_LO	1

void IRs_InitTransmitMode()
{
// Initialization of Transmit Here
IRs_TransmitInversion=IRS_TRANSMIT_HI;
// TODO: Enable Transmit mode
// TODO: Init PWM
}

//===============================

#define TMR_OVERFLOW 0xFFFF
#define SET_TMR_INTERRUPT(lo,hi) TMROH=TMR_OVERFLOW-(hi); TMR0L=TMR_OVERFLOW-(lo);

// Once Data is Received, process every two bytes only, this will be called once. then the next call for SET_TMR_INTERRUPT
// will be called by the interrupt routine
void IRs_DataReceived(byte hi,byte lo)
{
SET_TMR_INTERRUPT(lo,hi);
}

//===============================

//inside the interrupt (only section is shown)
#if 0

if(interrupt occurs)
	{
	if(IRs_TransmitInversion==IRS_TRANSMIT_HI)
		{
		// TODO: Transmit Hi
		IRs_TransmitInversion=IRS_TRANSMIT_LO;
		}
	else if(IRs_TransmitInversion==IRS_TRANSMIT_LO)
		{
		// TODO: Transmit Lo
		IRs_TransmitInversion=IRS_TRANSMIT_HI;
		}


	// TODO: get preceding bytes from the buffer
	if(lo==0xFF)&&(hi=0xFF)
		{
		// TODO:disable transmit mode
		// TODO:disable timer interrupt
		}
	else
		{SET_TMR_INTERRUPT(lo,hi);}
	}

#endif






//#endif //IR_IO