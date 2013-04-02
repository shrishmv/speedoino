/* Speedoino - This file is part of the firmware.
 * Copyright (C) 2011 Kolja Windeler
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "global.h"

/************************************************** Strategy: *******************************************************
 * On sending CAN Frame increase counter "can_missed_count" by one, max 100.
 * On receiving a CAN Frame, decrease it by two, so we accept up to 50% losses.
 * The can_missed_count should therefor indicate our traffic losses. Highvalue = high number of losses
 *
 * init_comm_possible() should be called in the beginning IF the sensor_source is set to AUTO_DETECT
 * init_comm_possible() can set the CAN_active to false, if the first 5 frames were send, but not even one came back
 *
 * get_CAN_missed_count() returns the CAN status. It will return 0 until we have more than 5 lost frames.
 * get_CAN_missed_count() returns SENSOR_OPEN as soon as we have more than 5 frames lost.
 *************************************************** Strategy *******************************************************/


Speedo_CAN::Speedo_CAN(){
	failed=false;
	message_available=false; // init with true, may be its one there
	last_request=0;
	last_received=0;
	can_speed=0;
	can_rpm=0;
	can_water_temp=0;
	can_dtc_errors_processed=0;
	can_mil_active=false;
	can_dtc_value=0x00;
	can_dtc_nr=0xff;
	can_dtc_error_count=0x00;
	can_missed_count=0;
};

Speedo_CAN::~Speedo_CAN(){
};

// Already in use in sensor class...
//ISR(PCINT2_vect ){
//	pSensors->check_inputs();
//}

void Speedo_CAN::init(){
	// Interrupt for CAN Interface active, auf pk4, pcint20

	// Chipselect as output
	if(pConfig->get_hw_version()==7){
		// Interrupt as Input with pull up
		CAN_DDR_CS_TILL_V7 &= ~(1<<CAN_INTERRUPT_PIN_V7); // input
		CAN_PORT_CS_TILL_V7 |= (1<<CAN_INTERRUPT_PIN_V7); //  active low => pull up
		// CS as output
		CAN_DDR_CS_TILL_V7 |= (1<<CAN_PIN_CS_TILL_V7);
	} else if(pConfig->get_hw_version()>7){
		// CS Input with pull up
		CAN_DDR_CS_FROM_V8 &= ~(1<<CAN_INTERRUPT_PIN_FROM_V8); // input
		CAN_PORT_CS_FROM_V8 |= (1<<CAN_INTERRUPT_PIN_FROM_V8); //  active low => pull up
		// CS as output
		CAN_DDR_CS_FROM_V8 |= (1<<CAN_PIN_CS_FROM_V8);
	};


	/********************************************* MCP2515 SETUP ***********************************/
	// MCP2515 per Software Reset zuruecksetzten,
	// danach ist der MCP2515 im Configuration Mode
	set_cs_high(false); //CS low
	spi_putc( SPI_CMD_RESET );
	_delay_ms(1);
	set_cs_high(true); //CS high

	// etwas warten bis sich der MCP2515 zurueckgesetzt hat
	_delay_ms(10);
	/******************** BAUD Rate ************************************************
	 *  Einstellen des Bit Timings
	 *
	 *  Fosc       = 16MHz
	 *  Bus Speed  = 500 kHz
	 *
	 *  Sync Seg   = 1TQ
	 *  Prop Seg   = (PRSEG + 1) * TQ  = 1 TQ
	 *  Phase Seg1 = (PHSEG1 + 1) * TQ = 3 TQ
	 *  Phase Seg2 = (PHSEG2 + 1) * TQ = 3 TQ
	 *
	 *  Bus speed  = 1 / (Total # of TQ) * TQ
	 *  500kHz= 1 / (8 * TQ)
	 * 	TQ = 1/(500kHz*8)
	 *  TQ = 2 * (BRP + 1) / Fosc
	 *  1/(500kHz*8) = 2 * (BRP + 1) / Fosc
	 *  1/(500kHz*8) = 2 * (BRP + 1) / 16000kHz
	 *  16000kHz/(500kHz*8*2) - 1 = BRP
	 *
	 *  BRP        = 1
	 ******************** BAUD Rate ************************************************/

	// BRP = 1
	mcp2515_write_register( CNF1, (1<<BRP0));
	// Prop Seg und Phase Seg1 einstellen
	mcp2515_write_register( CNF2, (1<<BTLMODE)|(1<<PHSEG11) );
	// Wake-up Filter deaktivieren, Phase Seg2 einstellen
	mcp2515_write_register( CNF3, (1<<PHSEG21) );
	// Aktivieren der Rx Buffer Interrupts
	mcp2515_write_register( CANINTE, (1<<RX1IE)|(1<<RX0IE) );

	/******************** FILTER ************************************************
	 * There are two input buffer: RXB0/1
	 * RXB0 has two Filters RXF0 / REF1
	 * RXB1 has four Filters RXF2 / REF3 / REF4 / REF5
	 *
	 * BUKT
	 * Additionally, the RXB0CTRL register can be configured
	 * such that, if RXB0 contains a valid message and
	 * another valid message is received, an overflow error
	 * will not occur and the new message will be moved into
	 * RXB1, regardless of the acceptance criteria of RXB1.
	 *
	 * RXBnCTRL.FILHITm determines if the Filter m is in use for buffer n
	 *
	 ******************** FILTER ************************************************/

	// old version to accept all MSG
	// mcp2515_write_register( RXB0CTRL, (1<<RXM1)|(1<<RXM0) );

	// BUKT: RXB0 message will rollover and be written to RXB1 if RXB0 is full
	// RXM0: Only accept messages with standard identifiers that meet filter criteria
	// FILHITx: RXF0 is active by deleting FILHIT
	// activate filter for buffer 0 and 1
	mcp2515_write_register( RXB0CTRL, (1<<BUKT)|(1<<RXM0)|(0<<FILHIT0));
	mcp2515_write_register( RXB1CTRL, (1<<RXM0));
	// TODO: build one filter for "wakeup" communication to buffer1, but for which ID ??

	// define filter RXF0/RXF1 and mask RXM0 to receive only frames with ID: 780-7FF
	// Filter: 111 1000 0000
	// Mask:   111 1000 0000
	// Accept: 111 1xxx xxxx <=> 780 - 7FF
	mcp2515_write_register( RXM0SIDH, (1<<SID10)|(1<<SID9)|(1<<SID8)|(1<<SID7)|(0<<SID6)|(0<<SID5)|(0<<SID4)|(0<<SID3));
	mcp2515_write_register( RXM0SIDL, 0x00);

	mcp2515_write_register( RXF0SIDH, (1<<SID10)|(1<<SID9)|(1<<SID8)|(1<<SID7)|(0<<SID6)|(0<<SID5));
	mcp2515_write_register( RXF0SIDL, 0x00);

	// thus i dont understand how to active just one filter, I'll simply copy them
	mcp2515_write_register( RXF1SIDH, (1<<SID10)|(1<<SID9)|(1<<SID8)|(1<<SID7)|(0<<SID6)|(0<<SID5));
	mcp2515_write_register( RXF1SIDL, 0x00);

	// extend addr filter leeren, nötig? wir verarbeiten ohnehin nur std adressen
	mcp2515_write_register( RXM0EID8, 0 );
	mcp2515_write_register( RXM0EID0, 0 );

	// TODO: build one filter for "wakeup" communication to buffer1, but for which ID ??
	// define filter RXF2/RXF3/RXF4/RXF5 and mask RXM1 to receive only frames with ID: 780-7FF
	// Filter: 111 1000 0000
	// Mask:   111 1000 0000
	// Accept: 111 1xxx xxxx <=> 780 - 7FF
	mcp2515_write_register( RXM1SIDH, (1<<SID10)|(1<<SID9)|(1<<SID8)|(1<<SID7)|(0<<SID6)|(0<<SID5)|(0<<SID4)|(0<<SID3));
	mcp2515_write_register( RXM1SIDL, 0x00);

	mcp2515_write_register( RXF2SIDH, (1<<SID10)|(1<<SID9)|(1<<SID8)|(1<<SID7)|(0<<SID6)|(0<<SID5));
	mcp2515_write_register( RXF2SIDL, 0x00);

	mcp2515_write_register( RXF3SIDH, (1<<SID10)|(1<<SID9)|(1<<SID8)|(1<<SID7)|(0<<SID6)|(0<<SID5));
	mcp2515_write_register( RXF3SIDL, 0x00);

	mcp2515_write_register( RXF4SIDH, (1<<SID10)|(1<<SID9)|(1<<SID8)|(1<<SID7)|(0<<SID6)|(0<<SID5));
	mcp2515_write_register( RXF4SIDL, 0x00);

	mcp2515_write_register( RXF5SIDH, (1<<SID10)|(1<<SID9)|(1<<SID8)|(1<<SID7)|(0<<SID6)|(0<<SID5));
	mcp2515_write_register( RXF5SIDL, 0x00);

	// extend addr filter leeren, nötig? wir verarbeiten ohnehin nur std adressen
	mcp2515_write_register( RXM1EID8, 0 );
	mcp2515_write_register( RXM1EID0, 0 );

	/*
	 *  Einstellen der Pin Funktionen
	 */
	// Deaktivieren der Pins RXnBF Pins (High Impedance State)
	mcp2515_write_register( BFPCTRL, 0 );
	// TXnRTS Bits als Inputs schalten
	mcp2515_write_register( TXRTSCTRL, 0 );
	// Device zurueck in den normalen Modus versetzten
	mcp2515_bit_modify( CANCTRL, 0xE0, 0);
	/********************************************* MCP2515 SETUP ***********************************/
	_delay_ms(1);
	request(CAN_CURRENT_INFO,CAN_RPM); // to check if can is pressent
};

void Speedo_CAN::shutdown(){

};

int Speedo_CAN::check_vars(){
	return 0; // return error code .. 1=failed, 0=ok
};

bool Speedo_CAN::init_comm_possible(bool* CAN_active){
	if(last_received==0){ // we have never ever received a pecket from CAN
		// starthilfe, nicht schön mit den zwei versionen ..
		if(pConfig->get_hw_version()==7){
			if(!CAN_INTERRUPT_PIN_PORT_V7&(1<<CAN_INTERRUPT_PIN_V7)){	 // if the CAN pin is low, low active interrupt
				message_available=true; // init with true, may be its one there
			}
		} else if(pConfig->get_hw_version()>7){
			if(!CAN_INTERRUPT_PIN_PORT_V8&(1<<CAN_INTERRUPT_PIN_FROM_V8)){	 // if the CAN pin is low, low active interrupt
				message_available=true; // init with true, may be its one there
			}
		}
		if(can_missed_count>10){ // see "strategy", in auto 10 lost frames indicates CAN offline, TODO: increase nr?
			*CAN_active=false;
			last_received=-1; // overrun, so it will never be possible to reactivate CAN.. wise?
			return false;
		} else {
			*CAN_active=true;
		}
	}
	return true;
}

/********************************************* CAN VALUE GETTER ***********************************/
int Speedo_CAN::get_air_temp(){
	return 0;
};

int Speedo_CAN::get_oil_temp(){
	return 0; // not in use
};

int Speedo_CAN::get_water_temp(){
	if(millis()-last_received<1000){
		return can_water_temp;
	} else {
		can_missed_count=999; // 1 sec no response ... CAN offline or not present
	}
	return 0;
};

int Speedo_CAN::get_CAN_missed_count(){
	if(can_missed_count>5){
		return SENSOR_OPEN;
	}
	return 0;
}

unsigned int Speedo_CAN::get_Speed(){
	if(millis()-last_received<1000){
		return can_speed;
	}
	return 0;
};

unsigned int Speedo_CAN::get_RPM(){
	if(millis()-last_received<500){
		return can_rpm;
	}
	return 0;
};


bool Speedo_CAN::get_mil_active(){
	return can_mil_active;
}

int Speedo_CAN::get_dtc_error_count(){
	return can_dtc_error_count;
}

int Speedo_CAN::get_dtc_error(int nr){
#ifdef CAN_DEBUG /////////////// DEBUG //////////
	Serial.print("gimme error nr");
	Serial.println(nr);
#endif/////////////// DEBUG //////////

	request(CAN_DTC,nr&0xff);
	unsigned long now=millis();
	while(millis()-now<1000 && can_dtc_errors_processed<=(unsigned)nr){ // if i processed more than I'm searching for
		if(message_available){
			process_incoming_messages();
			if(can_dtc_value>0){

#ifdef CAN_DEBUG /////////////// DEBUG //////////
				Serial.print("returning from get error");
				Serial.println(can_dtc_value);
#endif/////////////// DEBUG //////////

				return can_dtc_value;
			} else {
				now=millis(); // solange immerhin was zurück gekommen ist, geben wir ihm wieder zeit
			}
		}
	}
#ifdef CAN_DEBUG /////////////// DEBUG //////////
	Serial.println("returning from get error -1");
#endif/////////////// DEBUG //////////
	return -1;
}
/********************************************* CAN VALUE GETTER ***********************************/

/********************************************* CAN FUNCTIONS ***********************************/
void Speedo_CAN::request(char mode,char PID){
	byte can_first_byte=0x02; //frame im Datenstrom

	//check valid msg
	if(mode!=CAN_CURRENT_INFO && mode!=CAN_DTC){
		return;
	} else if(mode==CAN_DTC) {
		can_first_byte=0x01;
		can_dtc_errors_processed=0; //reset value since we are asing for a new one
		can_dtc_nr=PID; // we reuse PID to save the number of the error code we are locking for, so we can ask for up to 255 error codes
		can_dtc_value=0x00;
	} else if(mode==CAN_CURRENT_INFO){
		if(PID!=CAN_AIR_TEMP && PID!=CAN_WATER_TEMPERATURE && PID!=CAN_RPM && PID!=CAN_SPEED && PID!=CAN_MIL_STATUS){
			return;
		}
		can_first_byte=0x02;
	}

	// BUILD MESSAGE
	message.id = 0x07DF; // motor steuergerät .. TODO: Ist die fix?
	message.rtr = 0;

	message.length = 8;
	message.data[0] = can_first_byte;
	message.data[1] = mode; //mode 01==data, 03=dtc
	message.data[2] = PID;
	message.data[3] = 0x00; // fill up thus 8 byte are needed
	message.data[4] = 0x00;
	message.data[5] = 0x00;
	message.data[6] = 0x00;
	message.data[7] = 0x00;

	// Nachricht verschicken
	last_request=millis();
	can_send_message(&message);
};

void Speedo_CAN::process_incoming_messages(){
	while(can_get_message(&message)!=0xff){ //0xff=no more frames available
		if(can_missed_count>1){
			can_missed_count-=2;
		} else if(can_missed_count>0){
			can_missed_count-=1;
		};
		last_received=millis();
		//		Serial.println("New CAN Message found");
		if(message.length>2){ // must be at least 3 chars to check [2]
			if(message.data[1]==CAN_CURRENT_INFO+0x40){ // its 0x41 on a 0x01 question (current info)!!
				if(message.data[2]==CAN_RPM){
					can_rpm=(message.data[3]<<6)|(message.data[4]>>2);
#ifdef CAN_DEBUG /////////////// DEBUG //////////
					Serial.print("New RPM:");
					Serial.println(can_rpm);
#endif/////////////// DEBUG //////////
					return;
				} else if(message.data[2]==CAN_SPEED){
					can_speed=message.data[3];
#ifdef CAN_DEBUG /////////////// DEBUG //////////
					Serial.print("New speed:");
					Serial.println(can_speed);
#endif/////////////// DEBUG //////////
					return;
				} else if(message.data[2]==CAN_WATER_TEMPERATURE){
					can_water_temp=(message.data[3]-40)*10;
#ifdef CAN_DEBUG /////////////// DEBUG //////////
					Serial.print("New watertemp:");
					Serial.println(can_water_temp);
#endif/////////////// DEBUG //////////
					return;
				} else if(message.data[2]==CAN_MIL_STATUS){
					can_mil_active=((message.data[3]&0x80)>>7); // bit 7 (im msb?)
					can_dtc_error_count=message.data[3]&0x77;
#ifdef CAN_DEBUG /////////////// DEBUG //////////
					Serial.print("New MIL status:");
					Serial.println(can_dtc_error_count);
#endif/////////////// DEBUG //////////
					return;
				}
			} else if(message.data[1]==CAN_DTC+0x40){ // its 0x43 on a 0x03 question (current info)!!
				if(can_dtc_nr!=0xff){
#ifdef CAN_DEBUG /////////////// DEBUG //////////
					char buffer[30];
					sprintf(buffer,"DTC %02x %02x %02x %02x %02x %02x %02x %02x",message.data[0],message.data[1],message.data[2],message.data[3],message.data[4],message.data[5],message.data[6],message.data[7]);
					Serial.println(buffer);
#endif/////////////// DEBUG //////////

					// angeblich sind es 2 Byte pro code .. und 3 codes pro nachricht .. alle testen
					for(int i=0;i<3;i++){
						if(can_dtc_nr==can_dtc_errors_processed){
							/**************************** DTC decoding ****************************
							 * P,C,B,U and after that a four digit code
							 * we store it as integer, but it will be a signed and should be different to -1
							 *
							 * dtc can be abcd where a=[0,1,2], b=[0..9], c=[0..9], d=[0..9]
							 * so max is 2999 using just 12 bit leads to max 4095 .. remainig 4 bits is more then enough
							 *
							 */
							unsigned int temp_value=message.data[i*2+2]<<8 | message.data[i*2+3]; // 23, 45, 67
							//0x1202=0b 0001 0010 0000 0010
							can_dtc_value=(temp_value>>12)&0x3;						//0b0001 & 0b011 = 0b0001
							can_dtc_value=can_dtc_value*10+((temp_value>>8)&0x0f);	//0001 0010 & 0b1111 = 0010
							can_dtc_value=can_dtc_value*10+((temp_value>>4)&0x0f); 	//0001 0010 0000 & 0b1111 = 0000
							can_dtc_value=can_dtc_value*10+((temp_value>>0)&0x0f);	//0001 0010 0000 0010 & 0b000 = 0010

							// right now the code is stored human readable in the can_dtc_value using bit 0..11
							// shift character to bit 12,13
							// thus it was stored in bit 14 and 15 and should now be stored in 12 and 13 we just
							// shift it by 2. Mask the rest
							can_dtc_value|=(temp_value>>2)&0x3000;
							// Serial.print("Returing: ");
							// Serial.println(can_dtc_value);
							can_dtc_nr=0xff; // stop looking in this frames
						}
						can_dtc_errors_processed++;
					}
					return;
				}
			}
		}
#ifdef CAN_DEBUG /////////////// DEBUG //////////
		Serial.println("Warning - unprocessed message detected");
		char buffer[30];
		sprintf(buffer,"%02x %02x %02x %02x %02x %02x %02x %02x",message.data[0],message.data[1],message.data[2],message.data[3],message.data[4],message.data[5],message.data[6],message.data[7]);
		Serial.println(buffer);
#endif /////////////// DEBUG //////////
	}
	message_available=false; // all messages processed
}

bool Speedo_CAN::decode_dtc(char* char_buffer,char ECU_type){
	if(ECU_type==SPEED_TRIPPLE){
		if(pConfig->read(CONFIG_FOLDER,"SPEED_T.TXT",READ_MODE_TEXTREPLACEMENT,char_buffer)!=0){ // char_buffer serves two ways, incoming is "error code i'm looking for", outgoing "cleartext of error"
			sprintf(char_buffer,"Errortext not found");
			return false;
		};
	};
	return true;
}


uint8_t Speedo_CAN::spi_putc( uint8_t data ){
	// Sendet ein Byte
	SPDR = data;
	// Wartet bis Byte gesendet wurde
	while( !( SPSR & (1<<SPIF) ) ){};

	return SPDR;
}

void Speedo_CAN::mcp2515_write_register( uint8_t adress, uint8_t data ){
	// /CS des MCP2515 auf Low ziehen
	set_cs_high(false); //CS low

	spi_putc(SPI_CMD_WRITE);
	spi_putc(adress);
	spi_putc(data);

	// /CS Leitung wieder freigeben
	set_cs_high(true); //CS high
}

uint8_t Speedo_CAN::mcp2515_read_register(uint8_t adress){
	uint8_t data;

	// /CS des MCP2515 auf Low ziehen
	set_cs_high(false); //CS low

	spi_putc(SPI_CMD_READ);
	spi_putc(adress);

	data = spi_putc(0xff);

	// /CS Leitung wieder freigeben
	set_cs_high(true); //CS high

	return data;
}

void Speedo_CAN::mcp2515_bit_modify(uint8_t adress, uint8_t mask, uint8_t data){
	// /CS des MCP2515 auf Low ziehen
	set_cs_high(false); //CS low

	spi_putc(SPI_CMD_BIT_MODIFY);
	spi_putc(adress);
	spi_putc(mask);
	spi_putc(data);

	// /CS Leitung wieder freigeben
	set_cs_high(true); //CS high
}


uint8_t Speedo_CAN::can_send_message(CANMessage *p_message){
	uint8_t status, address;
	if(can_missed_count<100){
		can_missed_count++;
	};

	// Status des MCP2515 auslesen
	set_cs_high(false); //CS low
	spi_putc(SPI_CMD_READ_STATUS);
	status = spi_putc(0xff);
	spi_putc(0xff); // read dummy
	set_cs_high(true); //CS high

	/* Statusbyte:
	 *
	 * Bit  Funktion
	 *  2   TXB0CNTRL.TXREQ
	 *  4   TXB1CNTRL.TXREQ
	 *  6   TXB2CNTRL.TXREQ
	 */

	if (bit_is_clear(status, 2)) {
		address = 0x00;
	}
	else if (bit_is_clear(status, 4)) {
		address = 0x02;
	}
	else if (bit_is_clear(status, 6)) {
		address = 0x04;
	}
	else {
		/* Alle Puffer sind belegt,
           Nachricht kann nicht verschickt werden */
		return 0;
	}


	set_cs_high(false); //CS low

	spi_putc(SPI_CMD_WRITE_TX | address);

	// Standard ID einstellen
	spi_putc((uint8_t) (p_message->id>>3));
	spi_putc((uint8_t) (p_message->id<<5));

	// Extended ID
	spi_putc(0x00);
	spi_putc(0x00);

	uint8_t length = p_message->length;

	if (length > 8) {
		length = 8;
	}

	// Ist die Nachricht ein "Remote Transmit Request" ?
	if (p_message->rtr)
	{
		/* Ein RTR hat zwar eine Laenge,
           aber enthaelt keine Daten */

		// Nachrichten Laenge + RTR einstellen
		spi_putc((1<<RTR) | length);
	}
	else
	{
		// Nachrichten Laenge einstellen
		spi_putc(length);

		// Daten
		for (uint8_t i=0;i<length;i++) {
			spi_putc(p_message->data[i]);
		}
	}
	set_cs_high(true); //CS high

	asm volatile ("nop");

	/* CAN Nachricht verschicken
       die letzten drei Bit im RTS Kommando geben an welcher
       Puffer gesendet werden soll */

	set_cs_high(false); //CS low
	if (address == 0x00) {
		spi_putc(SPI_CMD_RTS | 0x01);
	} else {
		spi_putc(SPI_CMD_RTS | address);
	}
	set_cs_high(true); //CS high

	return 1;
}

uint8_t Speedo_CAN::mcp2515_read_rx_status(void){
	uint8_t data;

	// /CS des MCP2515 auf Low ziehen
	set_cs_high(false); //CS low

	spi_putc(SPI_CMD_RX_STATUS);
	data = spi_putc(0xff);

	// Die Daten werden noch einmal wiederholt gesendet,
	// man braucht also nur eins der beiden Bytes auswerten.
	spi_putc(0xff);

	// /CS Leitung wieder freigeben
	set_cs_high(true); //CS high

	return data;
}

uint8_t Speedo_CAN::can_get_message(CANMessage *p_message){
	// Status auslesen
	uint8_t status = mcp2515_read_rx_status();

	if (bit_is_set(status,6))
	{
		// Nachricht in Puffer 0

		set_cs_high(false); //CS low
		spi_putc(SPI_CMD_READ_RX);
	}
	else if (bit_is_set(status,7))
	{
		// Nachricht in Puffer 1

		set_cs_high(true); //CS high
		spi_putc(SPI_CMD_READ_RX | 0x04);
	}
	else {// TODO was ist denn mit Puffer 2 ?
		/* Fehler: Keine neue Nachricht vorhanden */
		return 0xff;
	}

	// Standard ID auslesen
	p_message->id =  (uint16_t) spi_putc(0xff) << 3; // 11 bit adressen
	p_message->id |= (uint16_t) spi_putc(0xff) >> 5;

	spi_putc(0xff);
	spi_putc(0xff);

	// Laenge auslesen
	uint8_t length = spi_putc(0xff) & 0x0f;
	if(length>8) length=8; // to avoid buffer overrun
	p_message->length = length;

	// Daten auslesen
	for (uint8_t i=0;i<length;i++) {
		p_message->data[i] = spi_putc(0xff);
	}

	set_cs_high(true); //CS high

	if (bit_is_set(status,3)) {
		p_message->rtr = 1;
	} else {
		p_message->rtr = 0;
	}

	// Interrupt Flag loeschen
	if (bit_is_set(status,6)) {
		mcp2515_bit_modify(CANINTF, (1<<RX0IF), 0);
	} else {
		mcp2515_bit_modify(CANINTF, (1<<RX1IF), 0);
	}
	return (status & 0x07);
}


// version depending pin settings
void Speedo_CAN::set_cs_high(bool high){
	if(high){
		// CS high
		if(pConfig->get_hw_version()==7){
			CAN_PORT_CS_TILL_V7 |= (1<<CAN_PIN_CS_TILL_V7);
		} else {
			CAN_PORT_CS_FROM_V8 |= (1<<CAN_PIN_CS_FROM_V8);
		}
	} else {
		// CS Low
		if(pConfig->get_hw_version()==7){
			CAN_PORT_CS_TILL_V7 &= ~(1<<CAN_PIN_CS_TILL_V7);
		} else {
			CAN_PORT_CS_FROM_V8 &= ~(1<<CAN_PIN_CS_FROM_V8);
		}
	}
}
/********************************************* CAN FUNCTIONS ***********************************/
