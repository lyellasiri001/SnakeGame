// Filename: SnakeProto.ino
// Author: S Harris, Lucky Yellasiri
// Date: 08/19/2025
// Description: SNAKE

#include <Arduino.h>

#include "freertos/FreeRTOS.h" // FreeRTOS
#include "freertos/task.h"

#include "LedControl.h" // LED screen
#include <binary.h> // for LED screen bitmap

#include <time.h>

// ==============================================
// Macros
// ==============================================
// Pins
#define LED_DATA_IN 11
#define LED_CLK 12
#define LED_CS 10

// Button pins
#define UP_BUTTON_PIN 1
#define LEFT_BUTTON_PIN 2
#define RIGHT_BUTTON_PIN 42
#define DOWN_BUTTON_PIN 41

// Game parameters
#define SCREEN_WIDTH 8
#define SCREEN_HEIGHT 8

// Bitwise operation macros for coordinates
#define UPPER_BITS(x) (x >> 4)
#define LOWER_BITS(x) (x & 0x0F)
#define TO_COORDINATES(x, y) (y | (x << 4))

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
#define SETUP_GAME 7

// Directions - UP and DOWN, LEFT and RIGHT form distinct pairs
#define UP 1
#define DOWN 2
#define RIGHT 10
#define LEFT 11

#define DEBOUNCE_MS 200
#define DIRECTION_QUEUE_SIZE 1

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
// Task Handles
// ==============================================

TaskHandle_t game;
TaskHandle_t led;

// ==============================================
// Queue Handle
// ==============================================

QueueHandle_t direction_queue;

// ==============================================
// Helpers
// ==============================================
// Checks the value (1 or 0) or a bit on the game bitmap, and returns 0 if
// it is 0, or a non-zero value if it is 1.
// Arguments:
//    - coordinates: coordinates formatted as a single uint8_t.
//                   The lower 4 bits are the y-coordinate, and the upper 4 are
//                   the x-coordinate.
uint8_t check_bitmap(uint8_t coordinates) {
  // We use double not to make sure non-0 values become 1
  return !!game_bitmap[LOWER_BITS(coordinates)] & (1 << UPPER_BITS(coordinates));
}

// Sets a bit on the game bitmap to 1.
// Arguments:
//    - coordinates: coordinates formatted as a single uint8_t.
//                   The lower 4 bits are the y-coordinate, and the upper 4 are
//                   the x-coordinate.
void set_bitmap_one(uint8_t coordinates) {
  game_bitmap[LOWER_BITS(coordinates)] |= (1 << UPPER_BITS(coordinates));
}

// Sets a bit on the game bitmap to 0.
// Arguments:
//    - coordinates: coordinates formatted as a single uint8_t.
//                   The lower 4 bits are the y-coordinate, and the upper 4 are
//                   the x-coordinate.
void set_bitmap_zero(uint8_t coordinates) {
  game_bitmap[LOWER_BITS(coordinates)] &= ~(1 << UPPER_BITS(coordinates));
}


// Picks a random unoccupied pixel for the apple to be placed in.
// Arguments:
//    - apple_coordinates: return parameter for coordinates of placed apple.
//    - current_score: the current game score.
// Warning: if all bits of the bitmap are 1, the contents of apple_coordinates
// will not be altered.
void place_apple(uint8_t* apple_coordinates, uint8_t current_score) {
  // Get a random value from 1 to (64 - current score)
  uint8_t r = rand() % (((SCREEN_HEIGHT * SCREEN_WIDTH) - 1 - current_score) + 1);
  // Iterate over the bitmap, counting off unoccupied (0) pixels
  // until we reach r. Since r's maximum value (63 - current score) is equal
  // to the number of pixels not currently lit (assuming there is not currently
  // an apple on screen), it should always end on a valid pixel for the apple.
  // This should ensure a relatively quick and evenly distributed "random"
  // placement for apples regardless of game state.
  for (uint8_t y = 0; y <= SCREEN_HEIGHT; y++) {
    for (uint8_t x = 0; x <= SCREEN_WIDTH; x++) { 
      if (r && !check_bitmap(TO_COORDINATES(x, y))) { // is this pixel/bit 0?
        r--;
      }
      if (!r) { // r has reached 0, place the apple
        *apple_coordinates = TO_COORDINATES(x, y);
        return;
      }
    }
  }

}



// Places a new head for the snake, based on the given old head and direction.
// If the coordinates exceed the screen dimensions in any direction, they will
// wrap back around to the other side of the screen.
// Arguments:
//     - old_head: the previous head of the snake. Upper 4 bits are the x
//                 coordinate; lower 4 are y.
//     - direction: the current direction the snake is moving in. Can be equal
//                  to the macros UP, DOWN, LEFT, and RIGHT.
// Returns the coordinates of the new head, in the same format as old_head.
uint8_t place_new_head(uint8_t old_head, uint8_t direction) {
  // Move our snake forward 1
  uint8_t x = UPPER_BITS(old_head) + (direction == RIGHT) - (direction == LEFT);
  // Instead of wall collision, wrap around
  if (x > SCREEN_WIDTH) { // unsigned, so -1 = 255
    x = SCREEN_WIDTH - 1;
  } else if (x == SCREEN_WIDTH) {
    x == 0;
  }
  uint8_t y = LOWER_BITS(old_head) + (direction == UP) - (direction == DOWN);
  if (y > SCREEN_HEIGHT) { // -1 = 255
    y = SCREEN_HEIGHT - 1;
  } else if (y == SCREEN_HEIGHT) {
    y = 0;
  }
  return TO_COORDINATES(x, y);
}

// ==============================================
// Snake Game Logic
// ==============================================
// 
void GameLogicTask(void *pvParameters) {
  uint8_t state = SETUP_GAME; // general game state (main menu is default)
  // Variables tracking the state of the game while actively playing
  uint8_t snake_buf[64]; // circular buffer tracking coordinates of snake
  uint8_t head_idx, tail_idx = 0; // head and tail idx of the buffer
  uint8_t current_head; // the coordinates of our current head
  uint8_t apple; // coordinates of the apple
  uint8_t current_score;
  bool got_apple = false; // did we get the apple last round?
  // Current direction snake is moving in
  uint8_t direction, body_direction;
  uint8_t new_direction; // store the next recieved direction to move in
  while (1) {
    //printf("starting game with state %d", state);
    if (state == MAIN_MENU) {
     /* snake_buf[0] = TO_COORDINATES(4, 1);
      set_bitmap_one(snake_buf[0]);
      game_bitmap[2] = 0xFF; // all 1s
      set_bitmap_zero(TO_COORDINATES(2, 2));
      for (int i = 0; i < SCREEN_WIDTH; i++) {
        Serial.println(game_bitmap[i], BIN);
      }*/
    } else if (state == SETUP_GAME) {
      Serial.println("setting up game!");
      for (int i =0; i < SCREEN_WIDTH; i++) {
        game_bitmap[i] = 0;
      }
      snake_buf[0] = TO_COORDINATES(4, 1);
      set_bitmap_one(snake_buf[0]);
      direction = UP;
      body_direction = UP;
      current_head = place_new_head(snake_buf[head_idx], direction);
      set_bitmap_one(current_head);
      apple = TO_COORDINATES(4, 7);
      set_bitmap_one(apple);
      current_score = 2; // size of snake
      state = PLAYING;
    } else if (state == PLAYING) {
      // main game logic
      // check if direction changed, change head position if so
      if (xQueueReceive(direction_queue, &new_direction, 0)) {
        // If this is a valid direction combination...
        if (((body_direction + new_direction) != (UP + DOWN)) && 
             ((body_direction + new_direction) != (LEFT + RIGHT))) {
           // Still need to stop turnaround from changing direction twice in one turn
           // (e.g. up / left / down)
           direction = new_direction;
          }
        // set the old head to 0
        set_bitmap_zero(current_head);
        // set our new direction
        current_head = place_new_head(snake_buf[head_idx], direction);
        set_bitmap_one(current_head);
      }      
      // Check for a collision with self or apple by checking the screen bitmap
      if (/*check_bitmap(current_head)*/current_head == apple) {
        apple = 0xFF; // out of bounds
        if (/*snake_buf[head_idx] == apple*/true) {
          got_apple = true; // set flag so we grow the tail next move
          current_score++;
          Serial.println("apple get");
          if (current_score >= 64) {
            state = GAME_OVER_TRANS;
          }
          // Erase the apple
          //set_bitmap_zero(apple);
        } else { // we collided with ourself, game over
          state = GAME_OVER_TRANS;
        }
      }
      if (ulTaskNotifyTake(pdTRUE,0)) {
        //uint8_t old_tail = tail_idx; // temporary
        // Move our snake forward 1
        head_idx = SNAKE_NEXT(head_idx);
        snake_buf[head_idx] = current_head;
        body_direction = direction;
        // Set the new controllable head of the snake
        current_head = place_new_head(snake_buf[head_idx], direction);
        set_bitmap_one(current_head);
        if (!got_apple) { // If we didn't get the apple this turn
          // Erase/redraw the tail
          set_bitmap_zero(snake_buf[tail_idx]);
          // Update tail coordinates by moving the tail index 1 forward
          tail_idx = SNAKE_NEXT(tail_idx);
        } else {
          Serial.print("tail should grow by 1, current score is ");
          Serial.println(current_score);
          // If we did get the apple, clear the flag for getting it
          // Note we do not move tail_idx forward here, so the snake grows by 1
          got_apple = false;
          // Get random coordinates for a new apple
          place_apple(&apple, current_score);
          // Draw the new apple
          set_bitmap_one(apple);
        }
      }
    } else if (state == GAME_OVER_TRANS) {
      // game over animation -- we don't have one for now, so just go straight to menu
      Serial.println("GAME OVER");
      Serial.print("YOUR SCORE: ");
      Serial.println(current_score);
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
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ==============================================
// Screen/LCD Display Tasks
// ==============================================
// 

void DisplayGameTask(void* pvParameters) {
  //uint8_t screen_buf[8]; // buffer for screen display
  while (1) {
    for (int i = 0; i < SCREEN_WIDTH; i++) {
      lc.setRow(0, i, game_bitmap[i]);
    }
    vTaskDelay(pdMS_TO_TICKS(25));
  }
}

// ==============================================
// Movement/ Movement Timing Tasks 
// ==============================================
// 

void SnakeMoveNoti(void* pvParameters) {
  while(1) {
    xTaskNotifyGive(game);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void SnakeButtonPressUp(void* pvParameters) {
  unsigned long last_time_pressed = millis();
  uint8_t direction = UP;
  while(1) {
   if (!digitalRead(UP_BUTTON_PIN) && (millis() - last_time_pressed) > DEBOUNCE_MS) {
    xQueueSend(direction_queue, &direction,  portMAX_DELAY);
    //Serial.print("UP");
   }
  vTaskDelay(pdMS_TO_TICKS(25));
  }
}

void SnakeButtonPressLeft(void* pvParameters) {
  unsigned long last_time_pressed = millis();
   uint8_t direction = LEFT;
  while(1) {
    if (!digitalRead(LEFT_BUTTON_PIN) && (millis() - last_time_pressed) > DEBOUNCE_MS) {
    xQueueSend(direction_queue, &direction ,portMAX_DELAY);
    //Serial.print("LEFT");
    }
    vTaskDelay(pdMS_TO_TICKS(25));
  }
}

void SnakeButtonPressRight(void* pvParameters) {
  unsigned long last_time_pressed = millis();
   uint8_t direction = RIGHT;
  while(1) {
    if (!digitalRead(RIGHT_BUTTON_PIN) && (millis() - last_time_pressed) > DEBOUNCE_MS) {
    xQueueSend(direction_queue, &direction, portMAX_DELAY);
    //Serial.print("RIGHT");
    }
    vTaskDelay(pdMS_TO_TICKS(25));
  }
}

void SnakeButtonPressDown(void* pvParameters) {
  unsigned long last_time_pressed = millis();
   uint8_t direction = DOWN;
  while(1) {
    if (!digitalRead(DOWN_BUTTON_PIN) && (millis() - last_time_pressed) > DEBOUNCE_MS) {
    xQueueSend(direction_queue, &direction, portMAX_DELAY);
    //Serial.print("DOWN");
    }
    vTaskDelay(pdMS_TO_TICKS(25));
  }
}


// ==============================================
// Setup
// ==============================================
void setup() {
  Serial.begin(115200);
  Serial.println("SNAKE");
  
  pinMode(UP_BUTTON_PIN, INPUT_PULLUP);
  pinMode(LEFT_BUTTON_PIN, INPUT_PULLUP);
  pinMode(RIGHT_BUTTON_PIN, INPUT_PULLUP);
  pinMode(DOWN_BUTTON_PIN, INPUT_PULLUP);

  lc.shutdown(0,false);
  /* Set the brightness to a medium values */
  lc.setIntensity(0,5);
  /* and clear the display */
  lc.clearDisplay(0);

  // Seed the RNG with the current time. Comment this out for deterministic apple placement.
  srand((int) micros());

  xTaskCreate(GameLogicTask, "GameLoop", 8192, NULL, 1, &game);
  xTaskCreate(DisplayGameTask, "screen", 8192, NULL, 1 , &led);
  xTaskCreate(SnakeMoveNoti, "move", 1024, NULL, 1, NULL);
  xTaskCreate(SnakeButtonPressUp, "up", 2048, NULL, 1, NULL);
  xTaskCreate(SnakeButtonPressLeft, "left", 2048, NULL, 1, NULL);
  xTaskCreate(SnakeButtonPressRight, "right", 2048, NULL, 1, NULL);
  xTaskCreate(SnakeButtonPressDown, "down", 2048, NULL, 1, NULL);

  direction_queue = xQueueCreate(DIRECTION_QUEUE_SIZE, sizeof(uint8_t));
}


// ==============================================
// Loop
// ==============================================
void loop() {
   //delay(10); // speed up sim
  // Leave empty. FreeRTOS handles scheduling.
}
