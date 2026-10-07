#include "gnarl.h"

#include <string.h>
#include <unistd.h>

#include <esp_timer.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "display.h"
#include "module.h"
#include "oled.h"

typedef struct {
	display_op_t op;
	int arg;
} display_command_t;

#define QUEUE_LENGTH	100

static QueueHandle_t display_queue;

static int connected = false;
static int phone_rssi;
static int pump_rssi;
static int command_time;  // seconds

// Keep the OLED on while debugging board bring-up.
#define DISPLAY_TIMEOUT	0  // seconds; 0 disables auto-off

static void format_time_ago(char *buf) {
	int now = esp_timer_get_time() / 1000000;
	int delta = now - command_time;
	int min = delta / 60;
	if (min == 0) {
		sprintf(buf, "%ds", delta);
		return;
	}
	if (min < 60) {
		sprintf(buf, "%dm", min);
		return;
	}
	int hr = min / 60;
	min = min % 60;
	sprintf(buf, "%dh%dm", hr, min);
}

static void format_uptime(char *buf) {
	int secs = esp_timer_get_time() / 1000000;
	sprintf(buf, "up %d:%02d:%02d", secs / 3600, (secs / 60) % 60, secs % 60);
}

static void render(void);

static void update(display_command_t cmd) {
	switch (cmd.op) {
	case PHONE_RSSI:
		phone_rssi = cmd.arg;
		ESP_LOGD(TAG, "phone RSSI = %d", phone_rssi);
		break;
	case PUMP_RSSI:
		pump_rssi = cmd.arg;
		ESP_LOGD(TAG, "pump RSSI = %d", pump_rssi);
		break;
	case COMMAND_TIME:
		command_time = cmd.arg;
		ESP_LOGD(TAG, "command time = %d", command_time);
		break;
	case CONNECTED:
		connected = cmd.arg;
		break;
	default:
		break;
	}
	render();
	if (DISPLAY_TIMEOUT > 0) {
		usleep(DISPLAY_TIMEOUT*SECONDS);
		oled_off();
	}
}

static void render(void) {
	oled_on();
	oled_clear();

	// Running uptime, refreshed once a second by display_loop.  If it stops
	// advancing the firmware is hung.
	oled_font_small();
	oled_align_center();
	char buf[16];
	format_uptime(buf);
	oled_draw_string(64, 9, buf);

	oled_font_medium();
	oled_align_center();
	oled_draw_string(64, 24, connected ? "Connected" : "Disconnected");

	oled_font_small();
	oled_align_left();
	oled_draw_string(5, 38, "Last command:");
	oled_draw_string(5, 50, "Phone RSSI:");
	oled_draw_string(5, 62, "Pump  RSSI:");

	oled_align_right();
	format_time_ago(buf);
	oled_draw_string(122, 38, buf);
	if (connected) {
		sprintf(buf, "%d", phone_rssi);
		oled_draw_string(122, 50, buf);
		sprintf(buf, "%d", pump_rssi);
		oled_draw_string(122, 62, buf);
	} else {
		oled_draw_string(122, 50, "--");
		oled_draw_string(122, 62, "--");
	}

	oled_update();
}

// Redraw once a second so the uptime clock stays live.  The render is cheap
// (a full u8g2 redraw over I2C takes ~16 ms), and it is the only way to tell
// a hung firmware from an idle one at a glance: a frozen clock means hung.
#define DISPLAY_REFRESH_MS	1000

static void display_loop(void *unused) {
	for (;;) {
		display_command_t cmd;
		if (xQueueReceive(display_queue, &cmd, pdMS_TO_TICKS(DISPLAY_REFRESH_MS))) {
			ESP_LOGD(TAG, "display_loop: op %d arg %d", cmd.op, cmd.arg);
			update(cmd);
		} else {
			render();
		}
	}
}

static void button_interrupt(void *unused) {
	display_command_t cmd = { .op = SHOW_STATUS };
	xQueueSendFromISR(display_queue, &cmd, 0);
}

void display_update(display_op_t op, int arg) {
	display_command_t cmd = { .op = op, .arg = arg };
	if (!xQueueSend(display_queue, &cmd, 0)) {
		ESP_LOGE(TAG, "display_update: queue full");
	}
}

void display_init(void) {
	oled_init();
	display_queue = xQueueCreate(QUEUE_LENGTH, sizeof(display_command_t));
	// 2048 bytes was too small: the u8g2 draw calls overflowed the task stack
	// ("A stack overflow in task display"), resetting the chip and dropping
	// BLE and RF.  ESP-IDF xTaskCreate takes the stack size in bytes.
	xTaskCreate(display_loop, "display", 8192, 0, 10, 0);
	display_update(SHOW_STATUS, 0);

	// Enable interrupt on button press.
	gpio_set_direction(BUTTON, GPIO_MODE_INPUT);
	gpio_set_intr_type(BUTTON, GPIO_INTR_NEGEDGE);
	gpio_install_isr_service(0);
	gpio_isr_handler_add(BUTTON, button_interrupt, 0);
	gpio_intr_enable(BUTTON);
}
