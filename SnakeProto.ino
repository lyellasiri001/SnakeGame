// Filename: SnakeProto.ino
// Author: S Harris, Lucky Yellasiri
// Date: 08/18/2025
// Description: SNAKE

#include <Arduino.h>

#include "freertos/FreeRTOS.h" // FreeRTOS
#include "freertos/task.h"

#include "LedControl.h" // LED screen
#include <binary.h> // for LED screen bitmap

// ==============================================
// Macros
// ==============================================
// Pins
#define LED_DATA_IN 5
#define LED_CLK 48
#define LED_CS 5

// Game parameters
#define SCREEN_WIDTH 8
#define SCREEN_HEIGHT 8

// Bitwise operation macros
#define UPPER_BITS(x) (x >> 4)
#define LOWER_BITS(x) (x & 0x0F)

// Modulo arithmetic for snake navigation
#define SNAKE_LEN 64
#define SNAKE_NEXT(x) ((x + 1) % SNAKE_LEN)

// Game states
#define MAIN_MENU 1
#define PLAYING 2
#define GAME_OVER_TRANS 3 // use if we want a death animation
#define GAME_OVER_MENU 4
#define ENTERING_HISCORE 5
#define VIEWING_HISCORE 6

// Directions
#define UP 1
#define DOWN 2
#define RIGHT 3
#define LEFT 4


// ==============================================
// Global variables
// ==============================================
// The LED board
// (args: data-in, clock, chip select, # of boards)
LedControl lc = LedControl(LED_DATA_IN, LED_CLK, LED_CS, 1); 
// NOTE: on any given turn of non-animation gameplay, we only need to update at most 3 pixels: snake head, snake tail, and apple.

uint8_t game_bitmap[8]; // state of the game screen to be displayed

// there's probably better ways to do this
const char* main_menu_strings[4] = {"PLAY", " PLAY       >", "HI-SCORES", " HI-SCORES  >"};
const char* hi_score_strings[2] = {"<    %s %d", "%s %d    %s %d"};
const char* enter_name_strings = {"   ENTER NAME"};

struct hi_score {
  char name[3];
  uint8_t score;
};

// Top 3 scores -- TODO in setup init the strings to "---"
struct hi_score hi_scores[3];

// ==============================================
// Task Handle(s)
// ==============================================
TaskHandle_t game;

// ==============================================
// Snake Game Logic
// ==============================================
// 
void GameLogicTask(void *pvParameters) {
  uint8_t state; // general game state
  // Variables tracking the state of the game while actively playing
  uint8_t snake_buf[64]; // circular buffer tracking coordinates of snake
  uint8_t head, tail = 0; // head and tail idx of the buffer
  uint8_t apple; // coordinates of the apple
  uint8_t head_x, head_y = 0; // current x and y of moving head
  uint8_t current_score;
  bool got_apple = false; // did we get the apple last round?
  // Current direction snake is moving in
  uint8_t direction;
  while (1) {
    if (state == MAIN_MENU) {
      // menu logic
    } else if (state == PLAYING) {
      // main game logic
      // check if direction changed, change head position if so
      if (false /*check for input to change direction*/) {
        // could probably write this better
      }
      // Move our snake forward 1
      head_x = UPPER_BITS(snake_buf[head]) + (direction == RIGHT) - (direction == LEFT);
      // Instead of wall collision, wrap around
      if (head_x > SCREEN_WIDTH) { // unsigned, so -1 = 255
        head_x = SCREEN_WIDTH - 1;
      } else if (head_x == SCREEN_WIDTH) {
        head_x == 0;
      }
      head_y = LOWER_BITS(snake_buf[head]) + (direction == UP) - (direction == DOWN);
      if (head_y > SCREEN_HEIGHT) { // -1 = 255
        head_y = SCREEN_HEIGHT - 1;
      } else if (head_y == SCREEN_HEIGHT) {
        head_y = 0;
      }
      if (false /*timer marks turn; time to move*/) {
        head = SNAKE_NEXT(head);
        snake_buf[head] = (head_x << 4) | (head_y);
        // only move the tail forward if we didn't get an apple last turn
        if (!got_apple) {
          tail = SNAKE_NEXT(tail);
        } else {
          got_apple = false; // reset
          // place the next apple somewhere we are not
          // possible algorithm: generate a "random" value 0 to (64 - score)
          // and iterate over open pixels by that amount?
          // ...
        }
      }
      // Check for a collision with self or apple by checking the screen bitmap
      if (game_bitmap[head_y] & (1 << head_x)) {
        if (snake_buf[head] == apple) {
          got_apple = true;
          current_score++;
        } else { // we collided with ourself, game over
          state = GAME_OVER_TRANS;
        }
      }
    } else if (state == GAME_OVER_TRANS) {
      // game over animation -- we don't have one for now, so just go straight to menu
      Serial.println("GAME OVER");
      state = GAME_OVER_MENU;
    } else if (state == GAME_OVER_MENU) {
      // menu logic      
    } else if (state == ENTERING_HISCORE) {
      // hi-score name entry logic      
    } else if (state == VIEWING_HISCORE) {
      // hi-score viewing logic
    } else {
      // error, invalid state
      Serial.println("Something is terribly wrong.");
    }
  }
}

// ==============================================
// Setup
// ==============================================
void setup() {
  Serial.begin(115200);
  lc.shutdown(0,false);
  /* Set the brightness to a medium values */
  lc.setIntensity(0,5);
  /* and clear the display */
  lc.clearDisplay(0);

  xTaskCreate(GameLogicTask, "GameLoop", 1024, NULL, 1, &game);
}


// ==============================================
// Loop
// ==============================================
void loop() {
   //delay(10); // speed up sim
  // Leave empty. FreeRTOS handles scheduling.
}