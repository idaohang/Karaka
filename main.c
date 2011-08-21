//***************************************************************************
//
//	File........: main.c
//	Description.: ATMega128 USB timer card.	 Interface between GPS module, 
//				  CCD camera, and Laptop
//	Copyright...: 2009-2011 Johnny McClymont, Paul Chote
//
//	This file is part of Karaka, which is free software. It is made available
//	to you under the terms of version 3 of the GNU General Public License, as
//	published by the Free Software Foundation. For more information, see LICENSE.
//
//***************************************************************************

#include <avr/interrupt.h>
#include "main.h"
#include "download.h"
#include "gps.h"
#include "display.h"
#include "command.h"

/* Hardware usage:
 * PINA:
 *    PINA0: output to camera download BNC Connector
 *    PINA1-5: input for unused hardware switch controls
 * PINB: unused
 * PINC: 8 bit output to LCD
 * PIND: 
 *    PIND0: PPS input from GPS
 *    PIND2: RS232 RX from GPS
 *    PIND3: RS232 TX to GPS
 * PINE:
 *    PINE0: RS232 TX to acquisition PC
 *    PINE1: RS232 RX from acquisition PC
 *    PINE4: NOT SCAN input from camera to monitor download status
 * PINF:
 *    PINF0: LCD register select output bit
 *    PINF1: LCD read/write select output bit
 *    PINF2: LCD enable output bit
 */
	
/*
 * Initialise the unit and wait for interrupts.
 */
int main(void)
{
	// Initialise global variables
	exposure_count = exposure_total = 0;
	exposure_syncing = TRUE;

	// Set INT0 to be rising edge triggered
    EICRA = (1<<ISC01)|(0<<ISC00);
    
	// Initialise the hardware units
	command_init();
	gps_init();
	download_init();
	display_init();
	
	// Enable interrupts
	sei();
	unsigned char time_updated;
	
	// Main program loop
	while(TRUE)
	{
		usart_process_buffer();
		time_updated = gps_process_buffer();
		
		if (exposure_total != 0)
            monitor_download();

		if (time_updated)
		{
			update_display();
		}
	}
}

/*
 * GPS time pulse interrupt handler
 * Fired when a `second boundary' time pulse is recieved via the attached BNC cable
 */
SIGNAL(SIG_INTERRUPT0)
{
	// Don't count down unless we have a valid exposure time, and the GPS is locked
	if (gps_state == GPS_TIME_GOOD && // Do we have a gps lock?
	    !exposure_syncing) // Are we waiting for a 10s boundary?
	{		
		// End of exposure - send a syncpulse to the camera
		// and store a flag so the gps can save the synctime.
		if (--exposure_count == 0)
		{
			exposure_count = exposure_total;
			gps_record_synctime = TRUE;
			trigger_download();
		}
	}
}
