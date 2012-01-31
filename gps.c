//***************************************************************************
//
//  File        : gps.c
//  Copyright   : 2009-2012 Johnny McClymont, Paul Chote
//  Description : Extracts timestamps from a Trimble or Magellan serial stream
//
//  This file is part of Karaka, which is free software. It is made available
//  to you under the terms of version 3 of the GNU General Public License, as
//  published by the Free Software Foundation. For more information, see LICENSE.
//
//***************************************************************************

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <stdio.h>
#include "main.h"
#include "display.h"
#include "gps.h"
#include "command.h"

// Init Trimble: Enable only the 8F-AB primary timing packet
unsigned char trimble_init[9] PROGMEM = {0x10, 0x8E, 0xA5, 0x00, 0x01, 0x00, 0x00, 0x10, 0x03};

unsigned char mgl_init[] PROGMEM =  "$PMGLI,00,G00,0,A\r\n"
                                    "$PMGLI,00,B00,0,A\r\n"
                                    "$PMGLI,00,B02,0,A\r\n"
                                    "$PMGLI,00,D00,0,A\r\n"
                                    "$PMGLI,00,E00,0,A\r\n"
                                    "$PMGLI,00,F02,0,A\r\n"
                                    "$PMGLI,00,R04,0,A\r\n"
                                    "$PMGLI,00,S01,0,A\r\n"
                                    "$PMGLI,00,A00,2,B\r\n"
                                    "$PMGLI,00,H00,2,B\r\n";

char gps_msg_missed_pps[]         PROGMEM = "Missing PPS pulse: forcing countdown";
char gps_msg_lost_serial[]        PROGMEM = "GPS serial connection lost";
char gps_msg_unknown_mgl_packet[] PROGMEM = "Unknown magellan packet";
char gps_msg_bad_packet[]         PROGMEM = "Bad GPS packet";
char gps_fmt_skipped_bytes[]      PROGMEM = "Skipped %d bytes while syncing";
char gps_fmt_checksum_failed[]    PROGMEM = "GPS Checksum failed. Got 0x%02x, expected 0x%02x";

static unsigned char gps_magellan_length = 0;
static unsigned char gps_magellan_locked = FALSE;

// NOTE: If buffer length is changed the read/write offsets
// must be changed to int, and explicit overflow code added
static unsigned char gps_input_buffer[256];
static unsigned char gps_input_read = 0;
static volatile unsigned char gps_input_write = 0;


#define GPS_PACKET_LENGTH 32
static unsigned char gps_packet_type = UNKNOWN_PACKET;
static unsigned char gps_packet_length = 0;
static unsigned char gps_packet[GPS_PACKET_LENGTH];

static unsigned char gps_output_buffer[256];
static volatile unsigned char gps_output_read = 0;
static volatile unsigned char gps_output_write = 0;

/*
 * Add a byte to the send queue and start sending data if necessary
 */
static void queue_send_byte(unsigned char b)
{
    // Don't overwrite data that hasn't been sent yet
    while (gps_output_write == gps_output_read - 1);

    gps_output_buffer[gps_output_write++] = b;

    // Enable Transmit data register empty interrupt if necessary to send bytes down the line
    cli();
    if ((UCSR1B & _BV(UDRIE1)) == 0)
        UCSR1B |= _BV(UDRIE1);
    sei();
}

/*
 * data register empty interrupt to send a byte down the wire
 */
ISR(USART1_UDRE_vect)
{
    if(gps_output_write != gps_output_read)
        UDR1 = gps_output_buffer[gps_output_read++];

    // Ran out of data to send - disable the interrupt
    if(gps_output_write == gps_output_read)
        UCSR1B &= ~_BV(UDRIE1);
}

/*
 * Initialise the gps listener on USART1
 */
void gps_init()
{
    // Set baud rate to 9600
    UBRR1 = 0xCF;
    UCSR1A = _BV(U2X1);

    // Enable receive, transmit, data received interrupt
    UCSR1B = _BV(RXEN1)|_BV(TXEN1)|_BV(RXCIE1);

    // Set 8-bit data frame
    UCSR1C = _BV(UCSZ11)|_BV(UCSZ10);

    // Initialize timer1 to monitor GPS loss
    TCCR1A = 0x00;

    // Set prescaler to 1/1024: 64us per tick
    TCCR1B = _BV(CS10)|_BV(CS12);
    TIMSK1 |= _BV(TOIE1);
    TCNT1 = 0x0BDB; // Overflow after 62500 ticks: 4.0s

    gps_record_synctime = FALSE;
    gps_state = GPS_UNAVAILABLE;
}

void send_gps_config()
{
    // Send receiver config
    for (unsigned char i = 0; i < 9; i++)
        queue_send_byte(pgm_read_byte(&trimble_init[i]));

    unsigned char i = 0, b = pgm_read_byte(&mgl_init[0]);
    do
    {
        queue_send_byte(b);
        b = pgm_read_byte(&mgl_init[++i]);
    } while (b != '\0');
}

static void set_time(timestamp *t)
{
    // Enable the counter for the next PPS pulse
    if (countdown_mode == COUNTDOWN_TRIGGERED)
        countdown_mode = COUNTDOWN_ENABLED;
    else if (countdown_mode == COUNTDOWN_ENABLED)
    {
        // We should always receive the PPS pulse before the time packet
        cli();
        trigger_countdown();
        sei();
        send_debug_string_P(gps_msg_missed_pps);
    }

    gps_last_timestamp = *t;

    // Mark that we have a valid timestamp
    gps_state = GPS_ACTIVE;

    // Synchronise the exposure with the edge of a minute
    if (countdown_mode == COUNTDOWN_SYNCING && (gps_last_timestamp.seconds % exposure_total == 0))
        countdown_mode = COUNTDOWN_ENABLED;

    if (gps_record_synctime)
    {
        cli();
        gps_last_synctime = gps_last_timestamp;
        gps_record_synctime = FALSE;
        sei();
        
        send_downloadtimestamp();
    }
    send_timestamp();
}


/*
 * Haven't received any serial data in 4.0 seconds
 * The GPS has probably died
 */
ISR(TIMER1_OVF_vect)
{
    gps_state = GPS_UNAVAILABLE;
    send_debug_string_P(gps_msg_lost_serial);
}

/*
 * Received data from gps serial. Add to buffer
 */
ISR(USART1_RX_vect)
{
    // Reset timeout countdown
    TCNT1 = 0x0BDB;

    // Update status if necessary
    if (gps_state == GPS_UNAVAILABLE)
        gps_state = GPS_SYNCING;
    gps_input_buffer[gps_input_write++] = UDR1;
}

/*
 * Helper routine for determining whether a given year is a leap year
 */
static unsigned char is_leap_year(unsigned int year)
{
    if (year % 4) return 0;
    if (year % 100) return 1;
    return (year % 400) ? 0 : 1;
}

/*
 * Process any data in the received buffer
 * Parses at most one time packet - so must be called frequently
 * Returns true if the timestamp or status info has changed
 * Note: this relies on the gps_input_buffer being 256 chars long so that
 * the data pointers automatically overflow at 256 to give a circular buffer
 */

static unsigned char bytes_to_sync = 0;
unsigned char gps_process_buffer()
{
    // Take a local copy of gps_input_write as it can be modified by interrupts
    unsigned char temp_write = gps_input_write;
    
    // No new data has arrived
    if (gps_input_read == temp_write)
        return FALSE;
    
    // Sync to the start of a packet if necessary
    for (; gps_packet_type == UNKNOWN_PACKET && gps_input_read != temp_write; gps_input_read++, bytes_to_sync++)
    {
        // Magellan packet            
        if (gps_input_buffer[(unsigned char)(gps_input_read - 1)] == '$' &&
            gps_input_buffer[(unsigned char)(gps_input_read - 2)] == '$' &&
            // End of previous packet
            gps_input_buffer[(unsigned char)(gps_input_read - 3)] == 0x0A)
        {
            if (gps_input_buffer[gps_input_read] == 'A')
            {
                gps_packet_type = MAGELLAN_TIME_PACKET;
                gps_magellan_length = 13;
            }
            else if (gps_input_buffer[gps_input_read] == 'H')
            {
                gps_packet_type = MAGELLAN_STATUS_PACKET;
                gps_magellan_length = 16;
            }
            else // Some other Magellan packet - ignore it
            {
                send_debug_string_P(gps_msg_unknown_mgl_packet);
                continue;
            }

            if (bytes_to_sync > 3)
                send_debug_fmt_P(gps_fmt_skipped_bytes, bytes_to_sync);

            bytes_to_sync = 0;
            
            // Rewind to the start of the packet
            gps_input_read -= 2;
            break;
        }
        
        // Trimble
        if ( // Start of timing packet
            gps_input_buffer[gps_input_read] == 0xAB &&
            gps_input_buffer[(unsigned char)(gps_input_read - 1)] == 0x8F &&
            gps_input_buffer[(unsigned char)(gps_input_read - 2)] == DLE &&
            // End of previous packet
            gps_input_buffer[(unsigned char)(gps_input_read - 3)] == ETX &&
            gps_input_buffer[(unsigned char)(gps_input_read - 4)] == DLE)
        {
            gps_packet_type = TRIMBLE_PACKET;
            // Rewind to the start of the packet
            gps_input_read -= 2;
            break;
        }
    }
    
    switch (gps_packet_type)
    {
        case UNKNOWN_PACKET:
            // Still haven't synced to a packet
        return FALSE;
        case TRIMBLE_PACKET:
            // Write bytes into the packet buffer
            for (; gps_input_read != temp_write; gps_input_read++)
            {
                // Skip the padding byte that occurs after a legitimate DLE
                // Don't loop this: we want to parse the 3rd DLE if there are
                // 4 in a row
                if (gps_input_buffer[gps_input_read] == DLE &&
                    gps_input_buffer[(unsigned char)(gps_input_read - 1)] == DLE)
                    gps_input_read++;

                if (gps_input_read == temp_write)
                    break;

                gps_packet[gps_packet_length++] = gps_input_buffer[gps_input_read];
            
                // End of packet (Trimble timing packet is 21 bytes)
                if (gps_packet_length == 21)
                {
                    // Sanity check: Ensure the packet ends correctly
                    if (gps_packet[20] == ETX && gps_packet[19] == DLE)
                    {
                        set_time(&(timestamp){
                            .hours = gps_packet[14],
                            .minutes = gps_packet[13],
                            .seconds = gps_packet[12],
                            .day = gps_packet[15],
                            .month = gps_packet[16],
                            .year = ((gps_packet[17] << 8) & 0xFF00) | (gps_packet[18] & 0x00FF),
                            .locked = gps_packet[11] == 0x03 ? TRUE : FALSE
                        });
                    }
                    else
                    {
                        send_debug_string_P(gps_msg_bad_packet);
                        send_debug_raw(gps_packet, gps_packet_length);
                    }

                    // Reset for next packet
                    gps_packet_type = UNKNOWN_PACKET;
                    gps_packet_length = 0;
                    return TRUE;
                }
            }
        break;
        
        case MAGELLAN_TIME_PACKET:
        case MAGELLAN_STATUS_PACKET:
            for (; gps_input_read != temp_write; gps_input_read++)
            {
                // Store the packet
                gps_packet[gps_packet_length++] = gps_input_buffer[gps_input_read];
                
                // End of packet
                if (gps_packet_length == gps_magellan_length)
                {
                    unsigned char updated = FALSE;
                    // Check that the packet is valid.
                    // A valid packet will have the final byte as a linefeed (0x0A)
                    // and the second-to-last byte will be a checksum which will match
                    // the XORed bytes between the $$ and checksum.
                    //   gps_packet_length - 1 is the linefeed
                    //   gps_packet_length - 2 is the checksum byte
                    if (gps_packet[gps_packet_length-1] == 0x0A)
                    {
                        unsigned char csm = gps_packet[2];
                        for (int i = 3; i < gps_packet_length-2; i++)
                            csm ^= gps_packet[i];

                        // Verify the checksum
                        if (csm == gps_packet[gps_packet_length-2])
                        {
                            if (gps_packet_type == MAGELLAN_TIME_PACKET)
                            {
                                updated = TRUE;

                                // Correct for bad epoch offset
                                // Number of days in each month (ignoring leap years)
                                static unsigned char days[13] = {
                                    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
                                };

                                // Add 19 years and 229 days
                                unsigned int year = (((gps_packet[9] << 8) & 0xFF00) | (gps_packet[10] & 0x00FF)) + 19;
                                unsigned char month = gps_packet[8];
                                unsigned char day = gps_packet[7];
                                unsigned char correction = 229;

                                // Is this a leap year?
                                days[1] = is_leap_year(year) ? 29 : 28;

                                while (day + correction > days[month-1])
                                {
                                    if (++month > 12)
                                    {
                                        month = 1;
                                        year++;
                                        days[1] = is_leap_year(year) ? 29 : 28;
                                    }
                                    correction -= days[month-1];
                                }
                                day += correction;

                                set_time(&(timestamp){
                                    .hours = gps_packet[4],
                                    .minutes = gps_packet[5],
                                    .seconds = gps_packet[6],
                                    .day = day,
                                    .month = month,
                                    .year = year,
                                    .locked = gps_magellan_locked
                                });
                            }
                            else if (gps_packet_type == MAGELLAN_STATUS_PACKET) // Status packet
                            {
                                if (gps_magellan_locked != (gps_packet[13] == 6))
                                {
                                    gps_magellan_locked = (gps_packet[13] == 6);
                                    updated = TRUE;
                                }
                            }
                            else
                            {
                                send_debug_string_P(gps_msg_bad_packet);
                                send_debug_raw(gps_packet, gps_packet_length);
                            }
                        }
                        else
                        {
                            send_debug_fmt_P(gps_fmt_checksum_failed, csm, gps_packet[gps_packet_length-2]);
                            send_debug_raw(gps_packet, gps_packet_length);
                        }
                    }
                    else
                    {
                        send_debug_string_P(gps_msg_bad_packet);
                        send_debug_raw(gps_packet, gps_packet_length);
                    }

                    // Reset buffer for the next packet
                    gps_packet_type = UNKNOWN_PACKET;
                    gps_packet_length = 0;
                    return updated;
                }
            }
        break;
    }
    return FALSE;
}