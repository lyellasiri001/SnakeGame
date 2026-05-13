/**
 * @file Snake.ino
 * @authors: Lucky Yellasiri, S. Harris
 * @date: 08/22/2025
 * @brief An implementation of the game Snake on an ESP32.
 * @details This file implements the game Snake on an ESP32, using FreeRTOS
 * for scheduling and concurrency/shared resource management across 2 cores.
 * The game uses an 8x8 LED board and 2x16 LCD screen for display output, and
 * 4 buttons for D-pad style input. It includes basic snake gameplay, a main
 * menu, win/lose screens, and a high-score gallery.
 */

// ==============================================
// Libraries
// ==============================================

#include <Arduino.h>

// FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// LED screen
#include "LedControl.h"
#include <binary.h> // for bitmap

// LCD screen
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#include <time.h> // for srand(); currently unused

// ==============================================
// Macros
// ==============================================
// Pins for output peripherals
#define LED_DATA_IN 11 /**< LED screen SPI data pin */
#define LED_CLK 12 /**< SPI clock */
#define LED_CS 10 /**< SPI chip select */
#define SCL_PIN 5 /**< LCD screen I2C clock */
#define SDA_PIN 6 /**< I2C data */

// Pins for the D-pad buttons.
#define UP_BUTTON_PIN 1
#define LEFT_BUTTON_PIN 2
#define RIGHT_BUTTON_PIN 42
#define DOWN_BUTTON_PIN 41

// LED screen parameters
#define SCREEN_WIDTH 8 /**< LED screen width */
#define SCREEN_HEIGHT 8 /**< LED screen height */

/**
 * @brief Bitwise operation macros for coordinates.
 * @details We store coordinates for the LED screen as single chars,
 * using the upper 4 bits for x and the lower 4 for y to save space.
 */
#define GET_X(x) (x >> 4) /**< Extracts x coordinate */
#define GET_Y(x) (x & 0x0F) /**< Extracts y coordinate */
#define TO_COORDINATES(x, y) (y | (x << 4)) /**< Converts x, y to a char coordinate */

// Modulo arithmetic for snake navigation
#define SNAKE_LEN 64 /**< Length of our snake coordinate buffer */
#define SNAKE_NEXT(x) ((x + 1) % SNAKE_LEN) /**< Gets the next index in our circular buffer */

// Game states
#define MAIN_MENU_SETUP 0
#define MAIN_MENU 1
#define PLAYING 2
#define GAME_OVER_TRANS 3
#define ENTERING_HISCORE_SETUP 4
#define ENTERING_HISCORE 5
#define VIEWING_HISCORE 6
#define SETUP_GAME 7

// Directions
/**
 * @brief Values used in game logic to represent D-pad directions.
 * @note UP and DOWN, LEFT and RIGHT form distinct pairs
 * for convenience.
 */
#define UP 1
#define DOWN 2
#define RIGHT 10
#define LEFT 11

// For handling button inputs
#define DEBOUNCE_MS 200 /**< Minimum time between registered inputs of a single button */
#define DIRECTION_QUEUE_SIZE 1


// ==============================================
// Global variables
// ==============================================
// The LED board
// (args: data-in, clock, chip select, # of boards)
LedControl lc = LedControl(LED_DATA_IN, LED_CLK, LED_CS, 1); /**< The LED board */

// The LCD screen
LiquidCrystal_I2C lcd(0x27, 16, 2); /**< The LCD screen */

// Game state output
uint8_t game_bitmap[8]; /**< Bitmap representing the state of the game to be displayed on the LED screen */
char lcd_buf[2][17]; /**< Buffer for 2 lines of text to be displayed on the LCD screen. */

// For convenience
const char* hi_score_strings[2] = {"<    %s %d", "%s %d    %s %d"}; /**< Format strings for displaying hi-scores. */
const char* main_menu_strings[2] = {">PLAY   HISCORE", " PLAY  >HISCORE"}; /**< Strings for the main menu options. */

/** 
 * @brief Struct to represent hi-scores
 * @note This was meant to include a name (e.g. "AAA"), though this
 * implementation does not use it.
 */
struct hi_score {
  char name[4]; /**< e.g. AAA */
  uint8_t score;
};

// Top 3 scores
struct hi_score hi_scores[3]; /**< Top 3 scores */

// ==============================================
// Task Handles
// ==============================================
TaskHandle_t game;
TaskHandle_t led;
TaskHandle_t lcdscreen;

// ==============================================
// Queues
// ==============================================
QueueHandle_t direction_queue; /**< Queue for communicating between button input tasks and game logic */

// ==============================================
// Semaphores
// ==============================================
SemaphoreHandle_t xBinarySemaphore; /**< Semaphore for managing shared access to the game bitmap */

// ==============================================
// Helpers
// ==============================================
/**
 * @brief Checks the value of a bit on the game bitmap at the given coordinates.
 * @param coordinates Coordinates formatted as a single uint8_t.
 *                   The lower 4 bits are the y-coordinate, and the upper 4 are
 *                   the x-coordinate.
 * @returns 0 if the bit is 0, or 1 if it is 1. If the coordinates are out of
 * bounds, it return the maximum char value (0xFF).
 */
uint8_t check_bitmap(uint8_t coordinates) {
  if (GET_X(coordinates) > SCREEN_WIDTH 
      || GET_Y(coordinates) > SCREEN_HEIGHT) {
    // out of bounds
    return 0xFF;
  }
  // We use double not to make sure non-0 values become 1
  return !!(game_bitmap[GET_Y(coordinates)] & (1 << GET_X(coordinates)));
}

/**
 * @brief Sets a bit on the game bitmap to 1 at the given coordinates.
 * If the coordinates are out of bounds, does nothing.
 * @param coordinates Coordinates formatted as a single uint8_t.
 *                   The lower 4 bits are the y-coordinate, and the upper 4 are
 *                   the x-coordinate.
 */
void set_bitmap_one(uint8_t coordinates) {
  if (GET_X(coordinates) > SCREEN_WIDTH 
      || GET_Y(coordinates) > SCREEN_HEIGHT) {
    // out of bounds
    return;
  }
  if (xSemaphoreTake(xBinarySemaphore, portMAX_DELAY) == pdTRUE){
    // Set the designated bit to 1
    game_bitmap[GET_Y(coordinates)] |= (1 << GET_X(coordinates));
    xSemaphoreGive(xBinarySemaphore);
  }

}

/**
 * @brief Sets a bit on the game bitmap to 0 at the given coordinates.
 * If the coordinates are out of bounds, does nothing.
 * @param coordinates Coordinates formatted as a single uint8_t.
 *                   The lower 4 bits are the y-coordinate, and the upper 4 are
 *                   the x-coordinate.
 */
void set_bitmap_zero(uint8_t coordinates) {
  if (GET_X(coordinates) > SCREEN_WIDTH 
      || GET_Y(coordinates) > SCREEN_HEIGHT) {
    // out of bounds
    return;
  }
  if (xSemaphoreTake(xBinarySemaphore, portMAX_DELAY) == pdTRUE) {
    // Set the designated bit to 0
    game_bitmap[GET_Y(coordinates)] &= ~(1 << GET_X(coordinates));
    xSemaphoreGive(xBinarySemaphore);
  }
}


/**
 * @brief Picks a random unoccupied pixel for the apple to be placed in.
 * @param apple_coordinates return parameter for coordinates of placed apple.
 * @param current_score the current game score.
 * @note If all bits of the bitmap are 1, the contents of apple_coordinates
 * will not be altered.
 */
void place_apple(uint8_t* apple_coordinates, uint8_t current_score) {
  // Get a random value from 1 to (64 - current score)
  uint8_t r = rand() % ((SCREEN_HEIGHT * SCREEN_WIDTH) - 1 - current_score);
  // Iterate over the bitmap, counting off unoccupied (0) pixels
  // until we reach r. Since r's maximum value (63 - current score) is equal
  // to the number of pixels not currently lit (assuming there is not currently
  // an apple on screen), it should always end on a valid pixel for the apple.
  // This should ensure a relatively quick and evenly distributed "random"
  // placement for apples regardless of game state.
  for (uint8_t y = 0; y < SCREEN_HEIGHT; y++) {
    for (uint8_t x = 0; x < SCREEN_WIDTH; x++) {
      // If this pixel is off/unoccupied 
      if (!check_bitmap(TO_COORDINATES(x, y))) {
        if (r) { // r > 0
          r--;
        } else {
          *apple_coordinates = TO_COORDINATES(x, y);
          return;
        }
      }
    }
  }
}

// ==============================================
// Snake Game Logic
// ==============================================
/**
 * @brief Task handling the main game logic.
 * @details The bulk of our game logic. Recieves queued inputs from button
 * tasks and generally handles the internal state of the game, including menus
 * and gameplay logic, and updates the LED bitmap and LCD buffers for their
 * respective tasks to display for the user.
 * @param _pvParameters Unused, included for FreeRTOS
 */
void GameLogicTask(void* _pvParameters) {
  // State variables
  uint8_t state = MAIN_MENU_SETUP; // general game state (main menu is default)
  uint8_t menu_state = 0; // current selected menu item
  // Variables tracking the state of the game while actively playing
  uint8_t snake_buf[64]; // circular buffer tracking coordinates of snake
  uint8_t head_idx, tail_idx = 0; // head and tail idx of the buffer
  uint8_t apple; // coordinates of the apple
  uint8_t current_score;
  bool got_apple = false; // did we get the apple last round?
  uint8_t direction; // direction snake is moving in
  uint8_t new_direction; // used to recieve button inputs from queue
  // Initialize hi-scores before game init
  for (uint8_t i = 0; i < 3; i++) {
    //hi_scores[i].name = "---";
    hi_scores[i].score = 0;
  }
  while (1) {
    if (state == MAIN_MENU_SETUP) {
      // Set up main menu screen
      vTaskDelay(pdMS_TO_TICKS(50));
      strncpy(lcd_buf[0], "   - SNAKE -   ", 16);
      strncpy(lcd_buf[1], main_menu_strings[0], 16);
      xTaskNotifyGive(lcdscreen);
      state = MAIN_MENU;
    } else if (state == MAIN_MENU) {
      // Wait for user button input
      if (xQueueReceive(direction_queue, &new_direction, 1)) {
        if (menu_state == 0) { // play
          if (new_direction == RIGHT) {
            menu_state = 1;
            strncpy(lcd_buf[1], main_menu_strings[1], 16);
            xTaskNotifyGive(lcdscreen);
          } else if (new_direction == DOWN) { // select
            state = SETUP_GAME;
          }
        } else { // hiscore
          if (new_direction == LEFT) {
            menu_state = 0;
            strncpy(lcd_buf[1], main_menu_strings[0], 16);
            xTaskNotifyGive(lcdscreen);
          } else if (new_direction == DOWN) { // select
            state = VIEWING_HISCORE;
            snprintf(lcd_buf[0], 16, hi_score_strings[0], 
                                     "1)", hi_scores[2].score);
            snprintf(lcd_buf[1], 16, hi_score_strings[1], 
                                     "2)", hi_scores[1].score, 
                                     "3)", hi_scores[0].score);
            xTaskNotifyGive(lcdscreen);
          }
        }
      }
    } else if (state == SETUP_GAME) {
      // Serial.println("setting up game!");
      for (int i = 0; i < SCREEN_WIDTH; i++) {
        game_bitmap[i] = 0;
      }
      // Create our snake -- 2 pixels long
      snake_buf[0] = TO_COORDINATES(4, 1);
      snake_buf[1] = TO_COORDINATES(4, 2);
      set_bitmap_one(snake_buf[0]);
      set_bitmap_one(snake_buf[1]);
      tail_idx = 0;
      head_idx = 1;
      direction = UP;
      // Set up our first apple -- always in the same place
      apple = TO_COORDINATES(4, 6);
      set_bitmap_one(apple);
      current_score = 2; // size of snake
      strncpy(lcd_buf[0], "SCORE: 2", 16);
      strncpy(lcd_buf[1], "", 16);
      xTaskNotifyGive(lcdscreen);
      state = PLAYING;
    } else if (state == PLAYING) {
      // main game logic
      // check if direction changed, change head position if so
      if (xQueueReceive(direction_queue, &new_direction, 0)) {
        // If this is a valid direction combination...
        // (we don't allow the snake to directly reverse direction)
        if (((direction + new_direction) != (UP + DOWN)) && 
             ((direction + new_direction) != (LEFT + RIGHT))) {
           direction = new_direction;
          }
      }      
      if (ulTaskNotifyTake(pdTRUE, 0)) { // timer trigger
        // If we got the apple last round, place a new one
        if (got_apple) {
          // If we did get the apple, clear the flag for getting it
          // Note we do not move tail_idx forward here, so the snake grows by 1
          got_apple = false;
          // Get random coordinates for a new apple
          place_apple(&apple, current_score);
          // Draw the new apple
          set_bitmap_one(apple);
        }
        // Move our snake forward 1 and put the new coordinate in the buffer
        uint8_t x = GET_X(snake_buf[head_idx]) + (direction == RIGHT) - (direction == LEFT);
        uint8_t y = GET_Y(snake_buf[head_idx]) + (direction == UP) - (direction == DOWN);
        head_idx = SNAKE_NEXT(head_idx);
        snake_buf[head_idx] = TO_COORDINATES(x, y);
        // Check for a collision with self or apple by checking the screen bitmap
        // and check if we have gone out of bounds (also a game over condition)
        if (check_bitmap(snake_buf[head_idx])
            || x >= SCREEN_WIDTH // unsigned, so -1 = 255
            || y >= SCREEN_HEIGHT) {
          if (snake_buf[head_idx] == apple) {
            got_apple = true; // set flag so we grow the tail next move
            apple = 0xFF; // out of bounds
            // Update score
            current_score++;
            snprintf(lcd_buf[0], 16, "SCORE: %d", current_score);
            xTaskNotifyGive(lcdscreen);
            if (current_score >= (SCREEN_HEIGHT * SCREEN_WIDTH)) { // max score
              state = GAME_OVER_TRANS;
            }
          } else { // we collided with ourself, game over
            state = GAME_OVER_TRANS;
          }
        }
        if (state == GAME_OVER_TRANS) {
          // DO NOT continue the loop, go DIRECTLY to game over
          continue;
        }
        // If we did not collide or game over, render the new head of the snake
        set_bitmap_one(snake_buf[head_idx]);
        // Check if we should move the tail
        if (!got_apple) { // If we didn't get the apple this turn
          // Erase/redraw the tail
          set_bitmap_zero(snake_buf[tail_idx]);
          // Update tail coordinates by moving the tail index 1 forward
          tail_idx = SNAKE_NEXT(tail_idx);
        }
      }
    } else if (state == GAME_OVER_TRANS) {
      // game over
      strncpy(lcd_buf[0], "   GAME  OVER   ", 16);
      if (current_score > hi_scores[0].score) {
        strncpy(lcd_buf[1], " NEW HI-SCORE!! ", 16);
      }
      xTaskNotifyGive(lcdscreen);
      // Death animation -- snake disappears from tail to head
      uint8_t death_frames = current_score;
      while (death_frames > 1) {
        set_bitmap_zero(snake_buf[tail_idx]);
        tail_idx = SNAKE_NEXT(tail_idx);
        vTaskDelay(pdMS_TO_TICKS(100 - death_frames));
        death_frames--;
      }
      set_bitmap_zero(snake_buf[tail_idx]);
      vTaskDelay(pdMS_TO_TICKS(100));
      set_bitmap_one(snake_buf[tail_idx]);
      vTaskDelay(pdMS_TO_TICKS(100));
      set_bitmap_zero(snake_buf[tail_idx]);
      vTaskDelay(pdMS_TO_TICKS(100));
      set_bitmap_one(snake_buf[tail_idx]);
      vTaskDelay(pdMS_TO_TICKS(200));
      set_bitmap_zero(snake_buf[tail_idx]);
      vTaskDelay(pdMS_TO_TICKS(500));
      set_bitmap_zero(apple);
      vTaskDelay(pdMS_TO_TICKS(2000));
      // Check for a new hi-score, and update hi-scores if so
      if (current_score > hi_scores[0].score) {
        if (current_score > hi_scores[1].score) {
          if (current_score > hi_scores[2].score) {
            uint8_t temp = hi_scores[2].score;
            uint8_t temp2 = hi_scores[1].score;
            hi_scores[2].score = current_score;
            hi_scores[1].score = temp;
            hi_scores[0].score = temp2; 
          } else {
            uint8_t temp = hi_scores[1].score;
            hi_scores[1].score = current_score;
            hi_scores[0].score = temp;
          }
        } else {
          hi_scores[0].score = current_score;
        }
        snprintf(lcd_buf[0], 16, hi_score_strings[0], 
                                 "1)", hi_scores[2].score);
        snprintf(lcd_buf[1], 16, hi_score_strings[1], 
                                 "2)", hi_scores[1].score, 
                                 "3)", hi_scores[0].score);
        xTaskNotifyGive(lcdscreen);
        state = VIEWING_HISCORE;
      } else {
        state = MAIN_MENU_SETUP;
      } 
    } else if (state == VIEWING_HISCORE) {
      // Just wait for the player to exit
      if (xQueueReceive(direction_queue, &new_direction, 1)) {
        if ((new_direction == UP) || (new_direction == LEFT)) {
          state = MAIN_MENU_SETUP;
        }
      }
    } else {
      Serial.println("Something is terribly wrong.");
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ==============================================
// Screen/LCD Display Tasks
// ==============================================
/**
 * @brief Displays the contents of the game bitmap on the LED screen.
 * Uses a binary semaphore to manage shared access of the bitmap
 * with tasks like the primary game logic.
 * @param _pvParameters Unused, included for FreeRTOS
 */
void DisplayGameTask(void* _pvParameters) {
  while (1) {
    if (xSemaphoreTake(xBinarySemaphore, portMAX_DELAY) == pdTRUE) {
      for (uint8_t i = 0; i < SCREEN_WIDTH; i++) {
      lc.setRow(0, i, game_bitmap[i]);
      }
      xSemaphoreGive(xBinarySemaphore);
    }
    vTaskDelay(pdMS_TO_TICKS(20)); // 50 Hz
  }
}

/**
 * @brief Displays the contents of the LCD buffers on the LCD screen, but only
 * refreshes the screen when it recieves a notification from another task
 * telling it the buffer has been updated.
 * @param _pvParameters Unused, included for FreeRTOS
 */
void DisplayLCDScreen(void* _pvParameters) {
  lcd.backlight();
  while(1) {
    if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY)) {
        lcd.clear();
        lcd.print(lcd_buf[0]);
        lcd.setCursor(0, 1);
        lcd.print(lcd_buf[1]);
    }
    vTaskDelay(pdMS_TO_TICKS(20)); // 50 Hz
  }
}


// ==============================================
// Movement/ Movement Timing Tasks 
// ==============================================

// ====== Timing ================================

/**
 * Notifies the game task when it's time for the snake to move.
 * @param _pvParameters Unused, included for FreeRTOS
 */
void SnakeMoveNoti(void* _pvParameters) {
  while(1) {
    xTaskNotifyGive(game);
    vTaskDelay(pdMS_TO_TICKS(1000)); // adjust as desired for difficulty
  }
}

// ====== Movement ==============================
// These four tasks all send inputs to a queue to communicate with the main
// game logic task, telling it when a button has been pressed.
// They run at a high frequency to ensure high responsiveness to user input.

/**
 * @brief Queues inputs from the up button to the direction queue.
 * @param _pvParameters Unused, included for FreeRTOS
 */
void SnakeButtonPressUp(void* _pvParameters) {
  unsigned long last_time_pressed = millis();
  uint8_t direction = UP;
  while(1) {
   if (!digitalRead(UP_BUTTON_PIN) && (millis() - last_time_pressed) > DEBOUNCE_MS) {
    xQueueSend(direction_queue, &direction,  portMAX_DELAY);
   }
  vTaskDelay(pdMS_TO_TICKS(15)); // approx. 64 Hz
  }
}

/**
 * @brief Queues inputs from the left button to the direction queue.
 * @param _pvParameters Unused, included for FreeRTOS
 */
void SnakeButtonPressLeft(void* _pvParameters) {
  unsigned long last_time_pressed = millis();
  uint8_t direction = LEFT;
  while(1) {
    if (!digitalRead(LEFT_BUTTON_PIN) && (millis() - last_time_pressed) > DEBOUNCE_MS) {
      xQueueSend(direction_queue, &direction ,portMAX_DELAY);
    }
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}

/**
 * @brief Queues inputs from the right button to the direction queue.
 * @param _pvParameters Unused, included for FreeRTOS
 */
void SnakeButtonPressRight(void* _pvParameters) {
  unsigned long last_time_pressed = millis();
  uint8_t direction = RIGHT;
  while(1) {
    if (!digitalRead(RIGHT_BUTTON_PIN) && (millis() - last_time_pressed) > DEBOUNCE_MS) {
      xQueueSend(direction_queue, &direction, portMAX_DELAY);
    }
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}

/**
 * @brief Queues inputs from the down button to the direction queue.
 * @param _pvParameters Unused, included for FreeRTOS
 */
void SnakeButtonPressDown(void* _pvParameters) {
  unsigned long last_time_pressed = millis();
  uint8_t direction = DOWN;
  while(1) {
    if (!digitalRead(DOWN_BUTTON_PIN) && (millis() - last_time_pressed) > DEBOUNCE_MS) {
      xQueueSend(direction_queue, &direction, portMAX_DELAY);
    }
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}


// ==============================================
// Setup
// ==============================================
void setup() {
  Serial.begin(115200);
  // Initialize the LCD screen and buffers
  Wire.begin(SDA_PIN, SCL_PIN);
  lcd.init();
  lcd_buf[0][0] = ' ';
  lcd_buf[1][0] = ' ';

  // Set up pins for button inputs
  pinMode(UP_BUTTON_PIN, INPUT_PULLUP);
  pinMode(LEFT_BUTTON_PIN, INPUT_PULLUP);
  pinMode(RIGHT_BUTTON_PIN, INPUT_PULLUP);
  pinMode(DOWN_BUTTON_PIN, INPUT_PULLUP);

  // Wake up the LED screen, clear and set brightness to mid-level
  lc.shutdown(0,false);
  lc.setIntensity(0,5);
  lc.clearDisplay(0);

  // Seed the RNG with the current time. 
  // (Comment this out for deterministic apple placement.)
  srand((int) micros());

  // Initialize the semaphore
  xBinarySemaphore = xSemaphoreCreateBinary();
  xSemaphoreGive(xBinarySemaphore);
  // Initalize the queue
  direction_queue = xQueueCreate(DIRECTION_QUEUE_SIZE, sizeof(uint8_t));

  // Start our tasks
  xTaskCreatePinnedToCore(GameLogicTask, "GameLoop", 14336, NULL, 1, &game, 0);
  xTaskCreatePinnedToCore(DisplayGameTask, "screen", 9216, NULL, 1 , &led, 1);
  xTaskCreatePinnedToCore(SnakeMoveNoti, "move", 1024, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(DisplayLCDScreen, "lcdscreen", 4096, NULL, 1, &lcdscreen, 1);
  xTaskCreatePinnedToCore(SnakeButtonPressUp, "up", 2048, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(SnakeButtonPressLeft, "left", 2048, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(SnakeButtonPressRight, "right", 2048, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(SnakeButtonPressDown, "down", 2048, NULL, 1, NULL, 1);

}


// ==============================================
// Loop
// ==============================================
void loop() {
   //delay(10); // speed up sim
  // Leave empty. FreeRTOS handles scheduling.
}
