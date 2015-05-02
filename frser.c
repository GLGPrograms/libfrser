/*
 * This file is part of the libfrser project.
 *
 * Copyright (C) 2010,2011,2013,2015 Urja Rannikko <urjaman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include "main.h"
#include "uart.h"
#include "frser-cfg.h"
#include "frser-int.h"
#include "frser-flashapi.h"
#include "udelay.h"
#include "frser.h"
#include "typeu.h"

#ifdef FRSER_FEAT_NONSPI
// Sys_bytes = stack + bss vars.
#ifndef FRSER_SYS_BYTES
#define FRSER_SYS_BYTES 320
#endif
#define RAM_BYTES (RAMEND-RAMSTART+1)
/* The length of the operation buffer */
#define S_OPBUFLEN (RAM_BYTES-FRSER_SYS_BYTES-UART_BUFLEN-UARTTX_BUFLEN)
#else
/* Fake. */
#define S_OPBUFLEN 12
#endif

struct constanswer {
	uint8_t len;
	PGM_P data;
} __attribute__ ((__packed__));

#ifdef FRSER_FEAT_LAST_OP
static uint8_t last_op;
uint8_t get_last_op(void) {
	return last_op;
}
#define LAST_OP(x) do { last_op = (x); } while(0)
#else
#define LAST_OP(x)
#endif

#ifndef FRSER_FEAT_PRE_OPRX_HOOK
#define FRSER_FEAT_PRE_OPRX_HOOK()
#endif

#ifndef FRSER_FEAT_POST_OPRX_HOOK
#define FRSER_FEAT_POST_OPRX_HOOK()
#endif


/* Calculate a nice read-n max value so that it doesnt hurt performance, but
   doesnt allow the device to be accidentally left in an "infini-tx" mode.
   This is the amount of data it can send based on baud rate in 2 seconds rounded to kB. */

#define BYTERATE (BAUD/5)
#define KBPSEC ((BYTERATE+512)/1024)
#define RDNMAX (KBPSEC*1024)

/* Report -4 for safety (atleast -1 because our buffer cant be filled 100%) */
#define UART_RBUFLEN (UART_BUFLEN-4)

/* 0xFF, 0xFF, 0xBF, 0x01 */

const char PROGMEM ca_nop[1] = { S_ACK };
const char PROGMEM ca_iface[3] = { S_ACK, 0x01, 0x00 };
const char PROGMEM ca_bitmap[33] = { S_ACK, FRSER_BM_B0, FRSER_BM_B1, FRSER_BM_B2, FRSER_BM_B3 };
const char PROGMEM ca_pgmname[17] = "\x06" FRSER_NAME; /* Small hack to include S_ACK in the name. */
const char PROGMEM ca_serbuf[3] = { S_ACK, (UART_RBUFLEN)&0xFF, (UART_RBUFLEN>>8)&0xFF };
const char PROGMEM ca_syncnop[2] = { S_NAK, S_ACK };

const char PROGMEM ca_opbufsz[3] = { S_ACK, S_OPBUFLEN&0xFF, (S_OPBUFLEN>>8)&0xFF };
const char PROGMEM ca_wrnlen[4] = { S_ACK, 0x00, 0x01, 0x00 };
const char PROGMEM ca_rdnmaxlen[4] = { S_ACK, RDNMAX&0xFF, (RDNMAX>>8)&0xFF, (RDNMAX>>16)&0xFF };

#ifndef FRSER_FEAT_DYNPROTO
const char PROGMEM ca_bustypes[2] = { S_ACK, SUPPORTED_BUSTYPES };
#endif

#ifdef FRSER_FEAT_PARALLEL
const char PROGMEM ca_chipsize[2] = { S_ACK, FRSER_PARALLEL_BITS };
#endif

/* Commands with a const answer cannot have parameters */
const struct constanswer PROGMEM const_table[S_MAXCMD+1] = {
	{ 1, ca_nop },		// NOP
	{ 3, ca_iface },	// IFACE V
	{ 33, ca_bitmap },	// op bitmap
	{ 17, ca_pgmname },	// programmer name
	{ 3, ca_serbuf },	// serial buffer size
#ifdef FRSER_FEAT_DYNPROTO
	{ 0, NULL },		// bustypes
#else
	{ 2, ca_bustypes },
#endif

#ifdef FRSER_FEAT_PARALLEL
	{ 2, ca_chipsize },	// chip size
#else
	{ 0, NULL },
#endif

	{ 3, ca_opbufsz },	// operation buffer size
	{ 4, ca_wrnlen },	// write-n max len
	{ 0, NULL },		// read byte
	{ 0, NULL },		// read n
	{ 0, NULL },		// init opbuf
	{ 0, NULL },		// opbuf, write-1
	{ 0, NULL },		// opbuf, write-n
	{ 0, NULL },		// opbuf, delay
	{ 0, NULL },		// exec opbuf
	{ 2, ca_syncnop },	// sync nop
	{ 4, ca_rdnmaxlen },	// Read-n maximum len
	{ 0, NULL },		// Set bustype
	{ 0, NULL },		// SPI operation
	{ 0, NULL },		// SPI speed
	{ 0, NULL },		// set output drivers
	{ 1, ca_syncnop },	// JEDEC toggle rdy => NAK (discard that thing)
	{ 0, NULL },		// Poll
	{ 0, NULL }		// Poll w delay
};

const uint8_t PROGMEM op2len[S_MAXCMD+1] = {
	/* A table to get  parameter length from opcode if possible (if not 0) */
	0x00, 0x00, 0x00,	/* NOP, iface, bitmap */
	0x00, 0x00, 0x00,	/* progname, serbufsize, bustypes */
	0x00, 0x00, 0x00,	/* chipsize, opbufsz, query-n maxlen */
	0x03, 0x06, 0x00,	/* read byte, read n, init opbuf */
	0x04, 0x00, 0x04,	/* write byte, write n, write delay */
	0x00, 0x00, 0x00,	/* Exec opbuf, syncnop, max read-n */
	0x01, 0x06, 0x04,	/* Set used bustype, SPI op, spi-speed */
	0x01, 0x00, 0x04, 	/* output drivers, togglerdy(nakd), poll */
	0x08			/* poll+delay */
};

#ifdef FRSER_FEAT_S_BUSTYPE
static uint8_t last_set_bus_types;
#else
/* Read-only */
#define last_set_bus_types SUPPORTED_BUSTYPES
#endif

#ifdef FRSER_FEAT_PIN_STATE
static uint8_t last_set_pin_state;
#endif


static uint32_t buf2u24(uint8_t *buf) {
	u32_u u24;
	u24.b[0] = buf[0];
	u24.b[1] = buf[1];
	u24.b[2] = buf[2];
	u24.b[3] = 0;
	return u24.l;
}

#ifdef FRSER_FEAT_NONSPI

#define OPBUF_WRITENOP 0x00
#define OPBUF_WRITE1OP 0x01
#define OPBUF_DELAYOP 0x02
#define OPBUF_POLL 0x03
#define OPBUF_POLL_DLY 0x04

static uint8_t opbuf[S_OPBUFLEN];
static uint16_t opbuf_bytes = 0;

static uint8_t opbuf_addbyte(uint8_t c) {
	if (opbuf_bytes == S_OPBUFLEN) return 1;
	opbuf[opbuf_bytes++] = c;
	return 0;
}


static void do_cmd_opbuf_writeb(uint8_t *parbuf) {
	if (opbuf_addbyte(OPBUF_WRITE1OP)) goto nakret;
	if (opbuf_addbyte(parbuf[0])) goto nakret;
	if (opbuf_addbyte(parbuf[1])) goto nakret;
	if (opbuf_addbyte(parbuf[2])) goto nakret;
	if (opbuf_addbyte(parbuf[3])) goto nakret;
	SEND(S_ACK);
	return;
nakret:
	SEND(S_NAK);
	return;
}


static void do_cmd_opbuf_delay(uint8_t *parbuf) {
	if (opbuf_addbyte(OPBUF_DELAYOP)) goto nakret;
	if (opbuf_addbyte(parbuf[0])) goto nakret;
	if (opbuf_addbyte(parbuf[1])) goto nakret;
	if (opbuf_addbyte(parbuf[2])) goto nakret;
	if (opbuf_addbyte(parbuf[3])) goto nakret;
	SEND(S_ACK);
	return;
nakret:
	SEND(S_NAK);
	return;
}


static void do_cmd_opbuf_poll_dly(uint8_t *parbuf) {
	if (opbuf_addbyte(OPBUF_POLL_DLY)) goto nakret;
	if (opbuf_addbyte(parbuf[0])) goto nakret;
	if (opbuf_addbyte(parbuf[1])) goto nakret;
	if (opbuf_addbyte(parbuf[2])) goto nakret;
	if (opbuf_addbyte(parbuf[3])) goto nakret;
	if (opbuf_addbyte(parbuf[4])) goto nakret;
	if (opbuf_addbyte(parbuf[5])) goto nakret;
	if (opbuf_addbyte(parbuf[6])) goto nakret;
	if (opbuf_addbyte(parbuf[7])) goto nakret;
	SEND(S_ACK);
	return;
nakret:
	SEND(S_NAK);
	return;
	}


static void do_cmd_opbuf_poll(uint8_t *parbuf) {
	if (opbuf_addbyte(OPBUF_POLL)) goto nakret;
	if (opbuf_addbyte(parbuf[0])) goto nakret;
	if (opbuf_addbyte(parbuf[1])) goto nakret;
	if (opbuf_addbyte(parbuf[2])) goto nakret;
	if (opbuf_addbyte(parbuf[3])) goto nakret;
	SEND(S_ACK);
	return;
nakret:
	SEND(S_NAK);
	return;
	}


static void do_cmd_opbuf_writen(void) {
	uint8_t len;
	uint8_t plen = 3;
	uint8_t i;
	len = RECEIVE();
	RECEIVE();
	RECEIVE();
	if (opbuf_addbyte(OPBUF_WRITENOP)) goto nakret;
	if (opbuf_addbyte(len)) goto nakret;
	plen--; if (opbuf_addbyte(RECEIVE())) goto nakret;
	plen--; if (opbuf_addbyte(RECEIVE())) goto nakret;
	plen--; if (opbuf_addbyte(RECEIVE())) goto nakret;
	for(;;) {
		len--; if (opbuf_addbyte(RECEIVE())) goto nakret;
		if (len == 0) break;
	}
	SEND(S_ACK);
	return;

nakret:
	len += plen;
	for(i=0;i<len;i++) RECEIVE();
	SEND(S_NAK);
	return;

}


static void do_cmd_opbuf_exec(void) {
	uint16_t readptr;
	for(readptr=0;readptr<opbuf_bytes;) {
		uint8_t op;
		op = opbuf[readptr++];
		if (readptr >= opbuf_bytes) goto nakret;
		if ((op == OPBUF_WRITE1OP)||(op==OPBUF_WRITENOP)) {
			uint32_t addr;
			uint8_t len,i;
			if (op==OPBUF_WRITENOP) {
				len = opbuf[readptr++];
				if (readptr >= opbuf_bytes) goto nakret;
			} else {
				len = 1;
			}
			addr = buf2u24(opbuf+readptr);
			readptr += 3;
			if (readptr >= opbuf_bytes) goto nakret;
			for(i=0;;) {
				uint8_t c;
				c = opbuf[readptr++];
				if (readptr > opbuf_bytes) goto nakret;
				flash_write(addr,c);
				addr++;
				i++;
				if (i==len) break;
			}
			continue;
		}
		if (op == OPBUF_DELAYOP) {
			u32_u usecs;
			usecs.b[0] = opbuf[readptr++];
			usecs.b[1] = opbuf[readptr++];
			usecs.b[2] = opbuf[readptr++];
			usecs.b[3] = opbuf[readptr++];
			if (readptr > opbuf_bytes) goto nakret;
			udelay(usecs.l);
			continue;
		}
		if ((op == OPBUF_POLL)||(op == OPBUF_POLL_DLY)) {
			uint8_t tmp1, tmp2;
			uint32_t i = 0;
			u32_u usecs;
			usecs.l = 0;
			uint8_t details = opbuf[readptr++];
			uint32_t addr = buf2u24(opbuf+readptr);
			readptr += 3;
			if (readptr > opbuf_bytes) goto nakret;
			if (op == OPBUF_POLL_DLY ) {
				usecs.b[0] = opbuf[readptr++];
				usecs.b[1] = opbuf[readptr++];
				usecs.b[2] = opbuf[readptr++];
				usecs.b[3] = opbuf[readptr++];
				if (readptr > opbuf_bytes) goto nakret;
			}
			uint8_t mask = 1 << (details&7);
			if (details&0x10) {
				/* toggle mode */
			        tmp1 = flash_read(addr) & mask;
			} else {
				/* data wait mode */
				tmp1 = details&0x20 ? mask : 0;
			}
		        while (i++ < 0xFFFFFFF) {
		                if (usecs.l) udelay(usecs.l);
		                tmp2 = flash_read(addr) & mask;
                		if (tmp1 == tmp2) {
		                        break;
                		}
                		/* Only move in toggle mode */
		                if (details&0x10) tmp1 = tmp2;
        		}
			continue;
		}

		goto nakret;
	}
	opbuf_bytes = 0;
	SEND(S_ACK);
	return;
nakret:
	opbuf_bytes = 0;
	SEND(S_NAK);
	return;
}


static void do_cmd_opbuf_init(void) {
	opbuf_bytes = 0;
}


static void do_cmd_readbyte(uint8_t *parbuf) {
	uint8_t c;
	uint32_t addr;
	SEND(S_ACK);
	addr = buf2u24(parbuf);
	c = flash_read(addr);
	SEND(c);
}

static void do_cmd_readnbytes(uint8_t *parbuf) {
	uint32_t addr;
	uint32_t n;
	SEND(S_ACK);
	addr = buf2u24(parbuf);
	n = buf2u24(parbuf+3);
	if (n==0) n = ((uint32_t)1 << 24); /* Protocol detail, dont show it to flash_readn */
	flash_readn(addr,n);
}
#else /* FRSER_FEAT_NONSPI ^^ */
static uint32_t opbuf_delay_acc = 0;

static void do_cmd_opbuf_delay(uint8_t *parbuf) {
	u32_u usecs;
	usecs.l = 0;
	usecs.b[0] = parbuf[0];
	usecs.b[1] = parbuf[1];
	usecs.b[2] = parbuf[2];
	usecs.b[3] = parbuf[3];
	opbuf_delay_acc += usecs.l;
	SEND(S_ACK);
	return;
}

static void do_cmd_opbuf_init(void) {
	opbuf_delay_acc = 0;
}

static void do_cmd_opbuf_exec(void) {
	if (opbuf_delay_acc) udelay(opbuf_delay_acc);
	opbuf_delay_acc = 0;
}
#endif

#ifdef FRSER_FEAT_SPI
static void do_cmd_spiop(uint8_t *parbuf) {
	uint32_t sbytes;
	uint32_t rbytes;
	sbytes = buf2u24(parbuf);
	rbytes = buf2u24(parbuf+3);
	flash_spiop(sbytes,rbytes);
}

#ifdef FRSER_FEAT_SPISPEED
static void do_cmd_spispeed(uint8_t* parbuf) {
	u32_u hz;
	hz.b[0] = parbuf[0];
	hz.b[1] = parbuf[1];
	hz.b[2] = parbuf[2];
	hz.b[3] = parbuf[3];
	if (hz.l==0) { /* I think this spec is stupid. /UR */
		SEND(S_NAK);
		return;
	}
	u32_u new_hz;
	new_hz.l = spi_set_speed(hz.l);
	SEND(S_ACK);
	SEND(new_hz.b[0]);
	SEND(new_hz.b[1]);
	SEND(new_hz.b[2]);
	SEND(new_hz.b[3]);
}
#endif
#endif

static void do_cmd_set_proto(uint8_t v) {
#ifdef FRSER_FEAT_PIN_STATE
	if (last_set_pin_state) {
		flash_select_protocol(v);
	}
#else
	flash_select_protocol(v);
#endif
#ifdef FRSER_FEAT_S_BUSTYPE
	last_set_bus_types = v;
#endif
}

#ifdef FRSER_FEAT_PIN_STATE
static void do_cmd_pin_state(uint8_t v) {
	if (v) {
		flash_select_protocol(last_set_bus_types);
	} else {
		flash_set_safe();
	}
	last_set_pin_state = v;
}
#endif


void frser_main(void) {
#ifdef FRSER_FEAT_PIN_STATE
	last_set_pin_state = 1;
#endif
	LAST_OP(0xFE);

#ifdef FRSER_FEAT_UART_TIMEOUT
	jmp_buf uart_timeout;
#endif
	do_cmd_set_proto(SUPPORTED_BUSTYPES); // select any protocol you like, just select one.
	for(;;) {
		uint8_t parbuf[S_MAXLEN]; /* Parameter buffer */
		uint8_t a_len,p_len;
		uint8_t op;
		uint8_t i;
#ifdef FRSER_FEAT_UART_TIMEOUT
		uart_set_timeout(NULL);
#endif
		FRSER_FEAT_PRE_OPRX_HOOK();
		op = RECEIVE();
		FRSER_FEAT_POST_OPRX_HOOK();

#ifdef FRSER_FEAT_DBG_CONSOLE
		if (op == 0x20) { /* Space bar gives you your debug console. */
			ciface_main();
			continue;
		}
#endif
		if (op > S_MAXCMD) {
			/* Protect against out-of-bounds array read. */
			SEND(S_NAK);
			continue;
		}
#ifdef FRSER_FEAT_UART_TIMEOUT
		if (setjmp(uart_timeout)) {
			/* We might be in the middle of an SPI operation or otherwise
			   in a weird state. Re-init and hope for best. */
			do_cmd_set_proto(last_set_bus_types);
			SEND(S_NAK); /* Tell of a problem. */
			continue;
		}
#endif
		LAST_OP(op);

		a_len = pgm_read_byte(&(const_table[op].len));
		/* These are the simple query-like operations, we just reply from ProgMem: */
		/* NOTE: Operations that have a const answer cannot have parameters !!!    */
		if (a_len) {
			PGM_P data = (PGM_P)pgm_read_ptr(&(const_table[op].data));
			for(i=0;i<a_len;i++) {
				uint8_t c = pgm_read_byte(&(data[i]));
				SEND(c);
			}
			continue;
		}
#ifdef FRSER_FEAT_UART_TIMEOUT
		uart_set_timeout(&uart_timeout);
#endif

		p_len = pgm_read_byte(&(op2len[op]));
		for (i=0;i<p_len;i++) parbuf[i] = RECEIVE();

		/* These are the operations that need real acting upon: */
		switch (op) {
			default:
				SEND(S_NAK);
				break;
#ifdef FRSER_FEAT_NONSPI
			case S_CMD_R_BYTE:
				do_cmd_readbyte(parbuf);
				break;
			case S_CMD_R_NBYTES:
				do_cmd_readnbytes(parbuf);
				break;
			case S_CMD_O_WRITEB:
				do_cmd_opbuf_writeb(parbuf);
				break;
			case S_CMD_O_WRITEN:
				do_cmd_opbuf_writen();
				break;
			case S_CMD_O_POLL:
				do_cmd_opbuf_poll(parbuf);
				break;
			case S_CMD_O_POLL_DLY:
				do_cmd_opbuf_poll_dly(parbuf);
				break;
#endif

#ifdef FRSER_FEAT_DYNPROTO
			case S_CMD_Q_BUSTYPE: /* Dynamic bus types. */
				SEND(S_ACK);
				SEND(flash_plausible_protocols());
				break;
#endif

			case S_CMD_O_INIT:
				SEND(S_ACK);
				do_cmd_opbuf_init();
				/* Select protocol atleast once per flashrom invocation in
				   order to detect change of chip between flashrom runs */
				do_cmd_set_proto(last_set_bus_types);
				break;

			case S_CMD_O_DELAY:
				do_cmd_opbuf_delay(parbuf);
				break;
			case S_CMD_O_EXEC:
				do_cmd_opbuf_exec();
				break;
#ifdef FRSER_FEAT_S_BUSTYPE
			case S_CMD_S_BUSTYPE:
				SEND(S_ACK);
				do_cmd_set_proto(parbuf[0]);
				break;
#endif
#ifdef FRSER_FEAT_SPI
			case S_CMD_O_SPIOP:
				do_cmd_spiop(parbuf);
				break;
#ifdef FRSER_FEAT_SPISPEED
			case S_CMD_S_SPI_FREQ:
				do_cmd_spispeed(parbuf);
				break;
#endif
#endif /* SPI */

#ifdef FRSER_FEAT_PIN_STATE
			case S_CMD_S_PIN_STATE:
				SEND(S_ACK);
				do_cmd_pin_state(parbuf[0]);
				break;
#endif

		}
	}
}
