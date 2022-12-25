/**
 * Copyright (C) 2022 by Mahyar Koshkouei <mk@deltabeard.com>
 * 
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

// Peanut-GB emulator settings
#define ENABLE_LCD	1
#define ENABLE_SOUND	1
#define PEANUT_GB_HIGH_LCD_ACCURACY 1
#define PEANUT_GB_USE_BIOS 0

/* Use DMA for all drawing to LCD. Benefits aren't fully realised at the moment
 * due to busy loops waiting for DMA completion. */
#define USE_DMA		0

/**
 * Reducing VSYNC calculation to lower multiple.
 * When setting a clock IRQ to DMG_CLOCK_FREQ_REDUCED, count to
 * SCREEN_REFRESH_CYCLES_REDUCED to obtain the time required each VSYNC.
 * DMG_CLOCK_FREQ_REDUCED = 2^18, and SCREEN_REFRESH_CYCLES_REDUCED = 4389.
 * Currently unused.
 */
#define VSYNC_REDUCTION_FACTOR 16u
#define SCREEN_REFRESH_CYCLES_REDUCED (SCREEN_REFRESH_CYCLES/VSYNC_REDUCTION_FACTOR)
#define DMG_CLOCK_FREQ_REDUCED (DMG_CLOCK_FREQ/VSYNC_REDUCTION_FACTOR)

/* C Headers */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RP2040 Headers */
#include <hardware/pio.h>
#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <hardware/spi.h>
#include <hardware/sync.h>
#include <hardware/timer.h>
#include <hardware/vreg.h>
#include <pico/bootrom.h>
#include <pico/stdio.h>
#include <pico/stdlib.h>
#include <pico/multicore.h>
#include <sys/unistd.h>
#include <hardware/irq.h>

/* Project headers */
#include "hedley.h"
#include "minigb_apu.h"
#include "peanut_gb.h"
#include "mk_ili9225.h"
#include "i2s.h"
#include "gbcolors.h"

/* GPIO Connections. */
#define GPIO_UP		2
#define GPIO_DOWN	3
#define GPIO_LEFT	4
#define GPIO_RIGHT	5
#define GPIO_A		6
#define GPIO_B		7
#define GPIO_SELECT	8
#define GPIO_START	9
#define GPIO_CS		17
#define GPIO_CLK	18
#define GPIO_SDA	19
#define GPIO_RS		20
#define GPIO_RST	21
#define GPIO_LED	22

#if ENABLE_SOUND
	// Global variables for audio task
    // stream contains N=AUDIO_SAMPLES samples
    // each sample is 32 bits
    // (16 bits for the left channel + 16 bits for the right channel in stereo interleaved format)
    // This is intended to be played at AUDIO_SAMPLE_RATE Hz
	uint16_t *stream;
#endif

/* DMA channel for LCD communication. */
static uint dma_lcd;
/* Definition of ROM data variable. Must be declared like:
 * #include <pico/platform.h>
 * const unsigned char __in_flash("rom") rom[] = {
 * 	...
 * };
 */
extern const unsigned char rom[];
unsigned char rom_bank0[16384];
static uint8_t ram[32768];
static int lcd_line_busy = 0;
static palette_t palette;	// Colour palette

static struct
{
	unsigned a	: 1;
	unsigned b	: 1;
	unsigned select	: 1;
	unsigned start	: 1;
	unsigned right	: 1;
	unsigned left	: 1;
	unsigned up	: 1;
	unsigned down	: 1;
} prev_joypad_bits;

/* Multicore command structure. */
union core_cmd {
    struct {
	/* Does nothing. */
#define CORE_CMD_NOP		0
	/* Set line "data" on the LCD. Pixel data is in pixels_buffer. */
#define CORE_CMD_LCD_LINE	1
	/* Control idle mode on the LCD. Limits colours to 2 bits. */
#define CORE_CMD_IDLE_SET	2
	/* Set a specific pixel. For debugging. */
#define CORE_CMD_SET_PIXEL	3
	uint8_t cmd;
	uint8_t unused1;
	uint8_t unused2;
	uint8_t data;
    };
    uint32_t full;
};

struct gb_priv {
    uint32_t lcd_line_hashes[LCD_HEIGHT];
    uint dma_pixel_buffer_chan;
};
static struct gb_priv gb_priv = { 0 };

/* Pixel data is stored in here. */
static uint8_t pixels_buffer[LCD_WIDTH];

#define putstdio(x) write(1, x, strlen(x))

/* Functions required for communication with the ILI9225. */
void mk_ili9225_set_rst(bool state)
{
	gpio_put(GPIO_RST, state);
}

void mk_ili9225_set_rs(bool state)
{
	gpio_put(GPIO_RS, state);
}

void mk_ili9225_set_cs(bool state)
{
	gpio_put(GPIO_CS, state);
}

void mk_ili9225_set_led(bool state)
{
	gpio_put(GPIO_LED, state);
}

void mk_ili9225_spi_write16(const uint16_t *halfwords, size_t len)
{
	spi_write16_blocking(spi0, halfwords, len);
}

void mk_ili9225_delay_ms(unsigned ms)
{
	sleep_ms(ms);
}

/**
 * Returns a byte from the ROM file at the given address.
 */
uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
	(void) gb;
	if(addr < sizeof(rom_bank0))
		return rom_bank0[addr];

	return rom[addr];
}

/**
 * Returns a byte from the cartridge RAM at the given address.
 */
uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr)
{
	(void) gb;
	return ram[addr];
}

/**
 * Writes a given byte to the cartridge RAM at the given address.
 */
void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr,
		       const uint8_t val)
{
	ram[addr] = val;
}

/**
 * Ignore all errors.
 */
void gb_error(struct gb_s *gb, const enum gb_error_e gb_err, const uint16_t addr)
{
#if 1
	const char* gb_err_str[4] = {
			"UNKNOWN",
			"INVALID OPCODE",
			"INVALID READ",
			"INVALID WRITE"
		};
	printf("Error %d occurred: %s at %04X\n.\n", gb_err, gb_err_str[gb_err], addr);
//	abort();
#endif
}

void core1_irq_dma_lcd_end_transfer(void)
{
	mk_ili9225_write_pixels_end();
	__atomic_store_n(&lcd_line_busy, 0, __ATOMIC_SEQ_CST);
	dma_channel_acknowledge_irq0(dma_lcd);
}

#if ENABLE_LCD 
void core1_lcd_draw_line(const uint_fast8_t line)
{
	static uint16_t fb[LCD_WIDTH];

	//dma_channel_wait_for_finish_blocking(dma_lcd);
	for(unsigned int x = 0; x < LCD_WIDTH; x++)
	{
		fb[x] = palette[(pixels_buffer[x] & LCD_PALETTE_ALL) >> 4]
				[pixels_buffer[x] & 3];
	}

	//mk_ili9225_set_address(line + 16, LCD_WIDTH + 30);
	mk_ili9225_set_x(line + 16);

#if USE_DMA
	mk_ili9225_write_pixels_start();
	dma_channel_transfer_from_buffer_now(dma_lcd, &fb[0], LCD_WIDTH);
	do {
		__wfi();
	} while (dma_channel_is_busy(dma_lcd));
	__compiler_memory_barrier();
#else
	mk_ili9225_write_pixels(fb, LCD_WIDTH);
	__atomic_store_n(&lcd_line_busy, 0, __ATOMIC_SEQ_CST);
#endif
}

_Noreturn
void main_core1(void)
{
	static dma_channel_config c2;
	uint16_t clear_screen_colour = palette[2][3];
	union core_cmd cmd;

	/* Initialise and control LCD on core 1. */
	mk_ili9225_init();

	/* Initilise DMA transfer for clearing the LCD screen. */
	dma_lcd = dma_claim_unused_channel(true);
	c2 = dma_channel_get_default_config(dma_lcd);
	channel_config_set_transfer_data_size(&c2, DMA_SIZE_16);
	channel_config_set_dreq(&c2,DREQ_SPI0_TX);
	channel_config_set_read_increment(&c2, false);
	channel_config_set_write_increment(&c2, false);
	channel_config_set_ring(&c2, false, 0);

	/* Enable IRQ for wake on interrupt functionality. */
	irq_set_exclusive_handler(DMA_IRQ_0, core1_irq_dma_lcd_end_transfer);
	dma_channel_set_irq0_enabled(dma_lcd, true);
	irq_set_enabled(DMA_IRQ_0, true);

	/* Clear LCD screen. */
	mk_ili9225_write_pixels_start();
	dma_channel_configure(dma_lcd, &c2, &spi_get_hw(spi0)->dr, &clear_screen_colour,
			      SCREEN_SIZE_X*SCREEN_SIZE_Y+16, true);
	do {
		__wfi();
	} while (dma_channel_is_busy(dma_lcd));
	__compiler_memory_barrier();

	/* Set DMA transfer to be the length of a DMG line. */
	dma_channel_set_trans_count(dma_lcd, LCD_WIDTH, false);
	channel_config_set_read_increment(&c2, true);
	//dma_sniffer_enable(dma_lcd, 0, true);

	/* Set LCD window to DMG size. */
	mk_ili9225_set_window(16, LCD_HEIGHT + 15,
			      31, LCD_WIDTH + 30);
	mk_ili9225_set_address(16, LCD_WIDTH + 30);
	//mk_ili9225_set_x(15);

#if 0
	/* Clear GB Screen window. */
	mk_ili9225_write_pixels_start();
	dma_channel_set_trans_count(dma_lcd, LCD_HEIGHT*LCD_WIDTH+16, false);
	dma_channel_set_read_addr(dma_lcd, &green, true);
	/* TODO: Add sleeping wait. */
	dma_channel_wait_for_finish_blocking(dma_lcd);
	mk_ili9225_write_pixels_end();
#endif

	// Sleep used for debugging LCD window.
	//sleep_ms(1000);

	/* Handle commands coming from core0. */
	while(1)
	{
		cmd.full = multicore_fifo_pop_blocking();
		switch(cmd.cmd)
		{
		case CORE_CMD_LCD_LINE:
			core1_lcd_draw_line(cmd.data);
			break;

		case CORE_CMD_IDLE_SET:
			mk_ili9225_display_control(true, cmd.data);
			break;

		case CORE_CMD_NOP:
		default:
			break;
		}
	}

	HEDLEY_UNREACHABLE();
}
#endif

#if ENABLE_LCD
void lcd_draw_line(struct gb_s *gb, const uint8_t pixels[LCD_WIDTH],
		   const uint_fast8_t line)
{
	union core_cmd cmd;

	/* Wait until previous line is sent. */
	while(__atomic_load_n(&lcd_line_busy, __ATOMIC_SEQ_CST))
		tight_loop_contents();

	//memcpy(pixels_buffer, pixels, LCD_WIDTH);
	dma_hw->sniff_data = 0;
	//line_to_copy = line;
	dma_channel_set_read_addr(gb_priv.dma_pixel_buffer_chan, pixels, false);
	dma_channel_set_write_addr(gb_priv.dma_pixel_buffer_chan, pixels_buffer, true);
	dma_channel_wait_for_finish_blocking(gb_priv.dma_pixel_buffer_chan);

	if(gb_priv.lcd_line_hashes[line] == dma_hw->sniff_data)
		return;

	gb_priv.lcd_line_hashes[line] = dma_hw->sniff_data;

	/* Populate command. */
	cmd.cmd = CORE_CMD_LCD_LINE;
	cmd.data = line;

	__atomic_store_n(&lcd_line_busy, 1, __ATOMIC_SEQ_CST);
	multicore_fifo_push_blocking(cmd.full);
}
#endif

int main(void)
{
	static struct gb_s gb;
	enum gb_init_error_e ret;
	
	/* Overclock. */
	{
		const unsigned vco = 1596*1000*1000;	/* 266MHz */
		const unsigned div1 = 6, div2 = 1;

		vreg_set_voltage(VREG_VOLTAGE_1_15);
		sleep_ms(2);
		set_sys_clock_pll(vco, div1, div2);
		sleep_ms(2);
	}

	/* Initialise USB serial connection for debugging. */
	stdio_init_all();
	//(void) getchar();
	putstdio("INIT: ");

	/* Initialise GPIO pins. */
	gpio_set_function(GPIO_UP, GPIO_FUNC_SIO);
	gpio_set_function(GPIO_DOWN, GPIO_FUNC_SIO);
	gpio_set_function(GPIO_LEFT, GPIO_FUNC_SIO);
	gpio_set_function(GPIO_RIGHT, GPIO_FUNC_SIO);
	gpio_set_function(GPIO_A, GPIO_FUNC_SIO);
	gpio_set_function(GPIO_B, GPIO_FUNC_SIO);
	gpio_set_function(GPIO_SELECT, GPIO_FUNC_SIO);
	gpio_set_function(GPIO_START, GPIO_FUNC_SIO);
	gpio_set_function(GPIO_CS, GPIO_FUNC_SIO);
	gpio_set_function(GPIO_CLK, GPIO_FUNC_SPI);
	gpio_set_function(GPIO_SDA, GPIO_FUNC_SPI);
	gpio_set_function(GPIO_RS, GPIO_FUNC_SIO);
	gpio_set_function(GPIO_RST, GPIO_FUNC_SIO);
	gpio_set_function(GPIO_LED, GPIO_FUNC_SIO);

	gpio_set_dir(GPIO_UP, false);
	gpio_set_dir(GPIO_DOWN, false);
	gpio_set_dir(GPIO_LEFT, false);
	gpio_set_dir(GPIO_RIGHT, false);
	gpio_set_dir(GPIO_A, false);
	gpio_set_dir(GPIO_B, false);
	gpio_set_dir(GPIO_SELECT, false);
	gpio_set_dir(GPIO_START, false);
	gpio_set_dir(GPIO_CS, true);
	gpio_set_dir(GPIO_RS, true);
	gpio_set_dir(GPIO_RST, true);
	gpio_set_dir(GPIO_LED, true);
	gpio_set_slew_rate(GPIO_CLK, GPIO_SLEW_RATE_FAST);
	gpio_set_slew_rate(GPIO_SDA, GPIO_SLEW_RATE_FAST);
	
	gpio_pull_up(GPIO_UP);
	gpio_pull_up(GPIO_DOWN);
	gpio_pull_up(GPIO_LEFT);
	gpio_pull_up(GPIO_RIGHT);
	gpio_pull_up(GPIO_A);
	gpio_pull_up(GPIO_B);
	gpio_pull_up(GPIO_SELECT);
	gpio_pull_up(GPIO_START);

	/* Set SPI clock to use high frequency. */
	clock_configure(clk_peri, 0,
			CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
			125 * 1000 * 1000, 125 * 1000 * 1000);
	spi_init(spi0, 30*1000*1000);
	spi_set_format(spi0, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

	/* Initialise GB context. */
	memcpy(rom_bank0, rom, sizeof(rom_bank0));
	ret = gb_init(&gb, &gb_rom_read, &gb_cart_ram_read,
		      &gb_cart_ram_write, &gb_error, &gb_priv);
	putstdio("GB ");

	if(ret != GB_INIT_NO_ERROR)
	{
		printf("Error: %d\n", ret);
		goto sleep;
	}

	/* Update buttons state */
	gb.direct.joypad_bits.up=gpio_get(GPIO_UP);
	gb.direct.joypad_bits.down=gpio_get(GPIO_DOWN);
	gb.direct.joypad_bits.left=gpio_get(GPIO_LEFT);
	gb.direct.joypad_bits.right=gpio_get(GPIO_RIGHT);
	gb.direct.joypad_bits.a=gpio_get(GPIO_A);
	gb.direct.joypad_bits.b=gpio_get(GPIO_B);
	gb.direct.joypad_bits.select=gpio_get(GPIO_SELECT);
	gb.direct.joypad_bits.start=gpio_get(GPIO_START);

	/* manually assign a palette with button combo   */  

	if(!gb.direct.joypad_bits.a && !gb.direct.joypad_bits.up) {
		get_colour_palette(palette,0x10,0x05);	/* A + Up */
	} else if(!gb.direct.joypad_bits.a && !gb.direct.joypad_bits.down) {
		get_colour_palette(palette,0x07,0x00);	/* A + Down */
	} else if(!gb.direct.joypad_bits.a && !gb.direct.joypad_bits.right) {
		get_colour_palette(palette,0x1C,0x03);	/* A + Right */
	} else if(!gb.direct.joypad_bits.a && !gb.direct.joypad_bits.left) {
		get_colour_palette(palette,0x0D,0x05);	/* A + Left */
	} else if(!gb.direct.joypad_bits.b && !gb.direct.joypad_bits.up) {
		get_colour_palette(palette,0x19,0x03);	/* B + Up */
	} else if(!gb.direct.joypad_bits.b && !gb.direct.joypad_bits.down) {
		get_colour_palette(palette,0x1A,0x05);	/* B + Down */
	} else if(!gb.direct.joypad_bits.b && !gb.direct.joypad_bits.right) {
		get_colour_palette(palette,0x13,0x00);	/* B + Right */
	} else if(!gb.direct.joypad_bits.b && !gb.direct.joypad_bits.left) {
		get_colour_palette(palette,0x16,0x00);	/* B + Left (Game Boy Pocket Palette, shades of gray) */
	} else if(!gb.direct.joypad_bits.up) {
		get_colour_palette(palette,0x12,0x00);	/* Up */
	} else if(!gb.direct.joypad_bits.down) {
		get_colour_palette(palette,0x17,0x00);	/* Down */
	} else if(!gb.direct.joypad_bits.right) {
		get_colour_palette(palette,0x05,0x00);	/* Right */
	} else if(!gb.direct.joypad_bits.left) {
		get_colour_palette(palette,0x18,0x05);	/* Left */
	} else if(!gb.direct.joypad_bits.a && !gb.direct.joypad_bits.b) {
		get_colour_palette(palette,0xFF,0xFF);	/* A + B */
	} else {
		/* Automatically assign a colour palette to the game */
		char rom_title[16];
		auto_assign_palette(palette, gb_colour_hash(&gb),gb_get_rom_name(&gb,rom_title));
	}

#if ENABLE_LCD
	gb_init_lcd(&gb, &lcd_draw_line);
	{
		/* Start Core1, which processes requests to the LCD. */
		putstdio("CORE1 ");
		multicore_launch_core1(main_core1);

		/* initialise pixel buffer copy DMA. */
		static dma_channel_config dma_config;
		gb_priv.dma_pixel_buffer_chan = dma_claim_unused_channel(true);
		dma_config = dma_channel_get_default_config(gb_priv.dma_pixel_buffer_chan);
		channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
		channel_config_set_read_increment(&dma_config, true);
		channel_config_set_write_increment(&dma_config, true);
		dma_sniffer_enable(gb_priv.dma_pixel_buffer_chan, 0x0, true);
		channel_config_set_sniff_enable(&dma_config, true);

#if 0
		irq_set_exclusive_handler(DMA_IRQ_1, core0_irq_dma_lcd_line_copy);
		dma_channel_set_irq1_enabled(gb_priv.dma_pixel_buffer_chan, true);
		irq_set_enabled(DMA_IRQ_1, true);
#endif

		dma_channel_configure(
			gb_priv.dma_pixel_buffer_chan,
			&dma_config,
			pixels_buffer,
			0, /* We don't have a pointer to the pixel buffer here. */
			LCD_WIDTH/sizeof(uint32_t),
			false
		);
	}
	putstdio("LCD ");
	//gb.direct.interlace = 1;
#endif
#if ENABLE_SOUND
	// Allocate memory for the stream buffer
    stream=malloc(AUDIO_BUFFER_SIZE_BYTES);
    assert(stream!=NULL);
    memset(stream,0,AUDIO_BUFFER_SIZE_BYTES);  // Zero out the stream buffer
	
	// Initialize I2S sound driver
	i2s_config_t i2s_config = i2s_get_default_config();
	i2s_config.sample_freq=AUDIO_SAMPLE_RATE;
	i2s_config.dma_trans_count =AUDIO_SAMPLES;
	i2s_volume(&i2s_config,2);
	i2s_init(&i2s_config);
	
	// Initialize audio emulation
	audio_init();
	
	putstdio("AUDIO ");
#endif

	putstdio("\n> ");
	uint_fast32_t frames = 0;
	uint64_t start_time = time_us_64();
	while(1)
	{
		int input;

		gb.gb_frame = 0;

		do {
			__gb_step_cpu(&gb);
			tight_loop_contents();
		} while(HEDLEY_LIKELY(gb.gb_frame == 0));

		frames++;
#if ENABLE_SOUND
		audio_callback(NULL, stream, AUDIO_BUFFER_SIZE_BYTES);
		i2s_dma_write(&i2s_config, stream);
#endif

		/* Update buttons state */
		prev_joypad_bits.up=gb.direct.joypad_bits.up;
		prev_joypad_bits.down=gb.direct.joypad_bits.down;
		prev_joypad_bits.left=gb.direct.joypad_bits.left;
		prev_joypad_bits.right=gb.direct.joypad_bits.right;
		prev_joypad_bits.a=gb.direct.joypad_bits.a;
		prev_joypad_bits.b=gb.direct.joypad_bits.b;
		prev_joypad_bits.select=gb.direct.joypad_bits.select;
		prev_joypad_bits.start=gb.direct.joypad_bits.start;
		gb.direct.joypad_bits.up=gpio_get(GPIO_UP);
		gb.direct.joypad_bits.down=gpio_get(GPIO_DOWN);
		gb.direct.joypad_bits.left=gpio_get(GPIO_LEFT);
		gb.direct.joypad_bits.right=gpio_get(GPIO_RIGHT);
		gb.direct.joypad_bits.a=gpio_get(GPIO_A);
		gb.direct.joypad_bits.b=gpio_get(GPIO_B);
		gb.direct.joypad_bits.select=gpio_get(GPIO_SELECT);
		gb.direct.joypad_bits.start=gpio_get(GPIO_START);

		/* hotkeys (select + * combo)*/
		if(!gb.direct.joypad_bits.select) {
#if ENABLE_SOUND
			if(!gb.direct.joypad_bits.up && prev_joypad_bits.up) {
				/* select + up */
				i2s_increase_volume(&i2s_config);
			}
			if(!gb.direct.joypad_bits.down && prev_joypad_bits.down) {
				/* select + down */
				i2s_decrease_volume(&i2s_config);
			}
#endif
		}

		/* Serial monitor commands */ 
		input = getchar_timeout_us(0);
		if(input == PICO_ERROR_TIMEOUT)
			continue;

		switch(input)
		{
#if 0
		static bool invert = false;
		static bool sleep = false;
		static uint8_t freq = 1;
		static ili9225_color_mode_e colour = ILI9225_COLOR_MODE_FULL;

		case 'i':
			invert = !invert;
			mk_ili9225_display_control(invert, colour);
			break;

		case 'f':
			freq++;
			freq &= 0x0F;
			mk_ili9225_set_drive_freq(freq);
			printf("Freq %u\n", freq);
			break;
#endif
		case 'c':
		{
			static ili9225_color_mode_e mode = ILI9225_COLOR_MODE_FULL;
			union core_cmd cmd;

			mode = !mode;
			cmd.cmd = CORE_CMD_IDLE_SET;
			cmd.data = mode;
			multicore_fifo_push_blocking(cmd.full);
			break;
		}

		case 'i':
			gb.direct.interlace = !gb.direct.interlace;
			break;

		case 'f':
			gb.direct.frame_skip = !gb.direct.frame_skip;
			break;

		case 'b':
		{
			uint64_t end_time;
			uint32_t diff;
			uint32_t fps;

			end_time = time_us_64();
			diff = end_time-start_time;
			fps = ((uint64_t)frames*1000*1000)/diff;
			printf("Frames: %u\n"
				"Time: %lu us\n"
				"FPS: %lu\n",
				frames, diff, fps);
			stdio_flush();
			frames = 0;
			start_time = time_us_64();
			break;
		}

		case '\n':
		case '\r':
		{
			gb.direct.joypad_bits.start = 0;
			break;
		}

		case '\b':
		{
			gb.direct.joypad_bits.select = 0;
			break;
		}

		case '8':
		{
			gb.direct.joypad_bits.up = 0;
			break;
		}

		case '2':
		{
			gb.direct.joypad_bits.down = 0;
			break;
		}

		case '4':
		{
			gb.direct.joypad_bits.left= 0;
			break;
		}

		case '6':
		{
			gb.direct.joypad_bits.right = 0;
			break;
		}

		case 'z':
		case 'w':
		{
			gb.direct.joypad_bits.a = 0;
			break;
		}

		case 'x':
		{
			gb.direct.joypad_bits.b = 0;
			break;
		}

		case 'q':
			goto out;

		default:
			break;
		}
	}
out:

	puts("\nEmulation Ended");

	mk_ili9225_set_rst(true);
	reset_usb_boot(0, 0);

	/* Sleep forever. */
sleep:
	stdio_flush();
	while(1)
		__wfi();

	HEDLEY_UNREACHABLE();
}
