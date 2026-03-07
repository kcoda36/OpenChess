#include <Adafruit_NeoPixel.h>
#include "font5x7.h"

// ---------------------------
// Hardware
// ---------------------------
#define LED_PIN	1
#define LED_COUNT  64
#define BRIGHTNESS 180

#define SER_PIN	45
#define RCLK_PIN   47
#define SRCLK_PIN  46

const int ROW_PINS[8] = {42, 41, 40, 39, 38, 37, 36, 35};

#define BTN_PIN 10   // IO10 = start / reset

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_RGBW + NEO_KHZ800);

/**
 * Write a single character to the display using the 5x7 font defined in font5x7.h.
 * The character is drawn in the bottom-left corner of the board, and the remaining pixels are cleared.
 * 
 * @param c     The ASCII character to display
 * @param color The color to use for the character (default is white)
 * 
 * @return true if the character was successfully displayed, false if the character is out of bounds (not in the font)
 */
bool writeChar(char c, uint32_t color = strip.Color(0, 0, 0, 255)) {
	if( c < 32 || c > 128 ) return false;

	bool success = false;
	int index = c * FONT_CHAR_WIDTH; // Starting byte for character

	strip.clear();
	for(int y = 0; y < FONT_CHAR_WIDTH; y++) {
		for(int x = 0; x < FONT_CHAR_HEIGHT; x++) {
			if( FONT_BIT(index, y, x) ) {
				uint16_t idx = x * 8 + y;				
				strip.setPixelColor(idx, color);
				success = true;
			}
		}
	}
	strip.show();

	return success;
}

// ---------------------------
// Setup
// ---------------------------
void setup() {
	Serial.begin(115200);
	delay(500);
	Serial.println("TEXT TEST");

	pinMode(SER_PIN,   OUTPUT);
	pinMode(RCLK_PIN,  OUTPUT);
	pinMode(SRCLK_PIN, OUTPUT);
	for (int i = 0; i < 8; i++) pinMode(ROW_PINS[i], INPUT_PULLUP);
	pinMode(BTN_PIN, INPUT_PULLUP);

	randomSeed(analogRead(0));

	strip.begin();
	strip.setBrightness(BRIGHTNESS);
	strip.clear();
	strip.show();
}

// ---------------------------
// Loop
// ---------------------------
int count = 0;
void loop() {
	delay(1000);

	char c = '0' + (count % 10);
	Serial.print("Displaying character: ");
	Serial.println(c);

	writeChar(c); // Write almost character [ascii 32–127] to the display
	count++;
}
