#include <Arduino.h>

#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <SerialFlash.h>


/**

1) Open "CONFIG.TXT" raw txt file on SD card,
and build list of files and relative frequencies.

**/

#define usb Serial

AudioPlaySdWav           playSdWav1;
AudioOutputI2S           i2s1;
AudioConnection          patchCord1(playSdWav1, 0, i2s1, 0);


enum ErrorState {
  BOOT = 1000,
  RUNNING = 5000
};
static ErrorState current_state = BOOT;

bool have_sd = false;
bool have_config = false;
const unsigned int SD_RETRY_PERIOD_MS = 1000;
unsigned long long min_repeat_ms = 0;
unsigned long long max_repeat_ms = 0;
unsigned long long last_tried_sd = 0;
unsigned long long last_played = 0;

const unsigned int MAX_SOUNDS = 64;
const unsigned int MAX_FILENAME_LEN = 64;
unsigned int active_filenames = 0;
char filenames[MAX_SOUNDS * MAX_FILENAME_LEN];
unsigned int relative_occurances[MAX_SOUNDS];
unsigned int total_of_relative_occurances = 0;

// Playback state
bool ready_to_play = false;
unsigned long long next_play_time = 0;
unsigned int next_play_ind = 0;


bool led_state = 0;
const unsigned int LED_ON_MS = 500;
unsigned long long last_flipped_led = 0; 

void setup() {
  // initialize digital pin LED_BUILTIN as an output.
  pinMode(LED_BUILTIN, OUTPUT);
  for (int i=0; i<3; i++){
    digitalWrite(LED_BUILTIN, 1);
    delay(250);
    digitalWrite(LED_BUILTIN, 0);
    delay(250);
  }
  
  // Set up Audio memory.
  AudioMemory(8);

  // Random seed from unconnected pin.
  randomSeed(analogRead(0));

  have_sd = false;
}

bool load_config(){
  /* Try to read CONFIG.TXT from SD card:
     Ignore any characters after # on a line.
     First line should be two unsigned ints separated by
     a space: the min + max repeat durations in ms.
     Any subsequent non-comment line should be
     a filename, a space, and the relative weight
     of that file (as an unsigned int). */
  File config_file = SD.open ("CONFIG.TXT", FILE_READ);
  if (!config_file){
    Serial.println("Couldn't open config file.");
    return false;
  }

  // Find lines with min/max configs.
  while (config_file.available()){
    String line = config_file.readStringUntil('\n');
    if (line[0] != '#'){
      // Non-comment line, we have our data.
      line.trim();
      min_repeat_ms = line.toInt();
      break;
    }
  }
  while (config_file.available()){
    String line = config_file.readStringUntil('\n');
    if (line[0] != '#'){
      // Non-comment line, we have our data.
      line.trim();
      max_repeat_ms = line.toInt();
      break;
    }
  }

  // Iterate through remaining lines, collecting
  // filenames + relative occurances.
  while (config_file.available() && active_filenames < MAX_SOUNDS){
    String line = config_file.readStringUntil('\n');
    if (line[0] != '#'){
      // Non-comment line, we have some data.
      line.trim();
      int space = line.indexOf(" ");
      String filename = line.substring(0, space);
      int rel_occurance = line.substring(space+1, line.length()).toInt();

      if (filename.length() < MAX_FILENAME_LEN){
        Serial.println("Copying filename ");
        Serial.print(filename);
        strcpy(filenames + active_filenames*MAX_FILENAME_LEN, filename.c_str());
        relative_occurances[active_filenames] = rel_occurance;
        total_of_relative_occurances += rel_occurance;
        active_filenames += 1;
      } else {
        Serial.println("Rejecting too long filename: ");
        Serial.println(filename);
      }
    }
  }

  if (min_repeat_ms > 0 &&
      max_repeat_ms > 0 &&
      max_repeat_ms >= min_repeat_ms &&
      active_filenames > 0 &&
      total_of_relative_occurances > 0){
    // Passed a bunch of sanity checks; should be good to go.
    return true;
  } else {
    return false;
    Serial.println("Unable to load all params from file.");
  }
}

void pick_random_sound_and_delay(){
  next_play_time = millis() + random(min_repeat_ms, max_repeat_ms);
  // Weighted sample according to relative occurances: pick
  // a random value <= the total of relative occurances, and iterate
  // through collecting a cumulative sum. When the cumulative sum
  // is >= this random value, take the current item.
  unsigned long long int cdf_val = random(0, total_of_relative_occurances);
  unsigned long long int cumsum = 0;
  for (unsigned int i = 0; i < active_filenames; i++){
    cumsum += relative_occurances[i];
    if (cumsum >= cdf_val){
      next_play_ind = i;
      break;
    }
  }
  ready_to_play = true;

  usb.printf("Scheduled to play %s with occurance %d/%d in %d ms.", 
             (char *)(filenames + next_play_ind * MAX_FILENAME_LEN),
             relative_occurances[next_play_ind],
             total_of_relative_occurances,
             next_play_time - millis());
}

void loop() {
  unsigned long long t = millis();
  if (!have_sd && t >= last_tried_sd + SD_RETRY_PERIOD_MS){
    last_tried_sd = t;
    have_sd = SD.begin(BUILTIN_SDCARD);
    if (!have_sd){
      usb.println("Unable to access the SD card.");
    } else {
      // Try to load in
      have_config = load_config();
      if (!have_config){
        usb.println("Unable to read config from SD card.");
        have_sd = false;
      } else {
        usb.printf("Read SD card: min %d and max %d repeat ms", min_repeat_ms, max_repeat_ms);
        pick_random_sound_and_delay();
        current_state = RUNNING;
      }
    }
  }

  if (have_sd && have_config){
    if (ready_to_play && t >= next_play_time){
      usb.printf("Playing %s\n", filenames + next_play_ind*MAX_FILENAME_LEN);
      playSdWav1.play(filenames + next_play_ind*MAX_FILENAME_LEN);
      delay(50);
      last_played = t;
      ready_to_play = false;
    } else if (!playSdWav1.isPlaying() && t >= next_play_time + 100){
      pick_random_sound_and_delay();
    }
  }

  if (!led_state && t >= last_flipped_led + current_state){
    led_state = 1;
    last_flipped_led = t;
    digitalWrite(LED_BUILTIN, HIGH);
  } else if (led_state && t >= last_flipped_led + LED_ON_MS) {
    last_flipped_led = t;
    led_state = 0;
    digitalWrite(LED_BUILTIN, LOW);
  }
}
