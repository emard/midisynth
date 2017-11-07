#include <MIDI.h>

// drty workaround to get MIDI compiling
void* __dso_handle = (void*) &__dso_handle;
__BEGIN_DECLS
int __cxa_atexit(void (_destructor) (void *), void *arg, void *dso) { return (0);}
__END_DECLS;

// FOR testing with usb-midi:
// edit ~/Arduino/libraries/MIDI_Library/src/midi_Settings.h
// BaudRate = 115200
// start bridge from alsa to midi serial
// ttymidi -b 115200 -v -s /dev/ttyUSB0
// on vmpk soft-MIDI keyboard,
// Edit->Connections->Output MIDI connection:
// select "Midi Through:0" OK,
// then select "ttymidi:1" OK
// press vmpk keys and LED will turn on when a key is pressed

MIDI_CREATE_DEFAULT_INSTANCE();

#define LED 13
int led_indicator_pin = LED;
uint8_t led_value = 0;
volatile uint32_t *led_indicator_pointer; // address of LED MMIO register

volatile uint32_t *voice = (uint32_t *)0xFFFFFBB0; // voices
volatile uint32_t *pitch  = (uint32_t *)0xFFFFFBB4; // frequency for prev written voice
int16_t volume[128], target[128];
uint32_t freq[128]; // freqency list

// constants for frequency table caculation
const int C_clk_freq = 50000000; // Hz system clock
const float C_ref_freq = 440.0; // Hz reference tone
const int C_ref_octave = 5; // Octave 4 (MIDI voice 0 is C-1, octave -1)
const int C_ref_tone = 9; // Tone A
const int C_pa_data_bits = 32;
const int C_voice_addr_bits = 7; // 128 voices MIDI standard
const int C_tones_per_octave = 12; // tones per octave, MIDI standard
const float C_cents_per_octave = 1200.0; // cents (tuning) per octave

// Standard MIDI has equal temperament (100 cents each)
// Hammond tried to make it, but tonewheels were slightly off-tune
// because a wheel could have no more than 192 teeth.
// Real notes must be a little bit off-tune in order to sound correct :)
const float C_temperament[C_tones_per_octave] =
{ // Hammond temperament
         0.0,        //  0 C
        99.89267627, //  1 C#
       200.7760963,  //  2 D
       300.488157,   //  3 Eb
       400.180858,   //  4 E
       499.8955969,  //  5 F
       600.6025772,  //  6 F#
       700.5966375,  //  7 G
       799.8695005,  //  8 G#
       900.5764808,  //  9 A
      1000.29122,    // 10 Bb
      1099.983921    // 11 B
};

// calculate base frequency, this is lowest possible A, meantone_temperament #9
// downshifting by (C_voice_addr_bits+C_pa_data_bits) is moved to C_shift_octave to avoid floating underflow here
const float C_base_freq = C_clk_freq*pow(2.0,C_temperament[C_ref_tone]/C_cents_per_octave);
// calculate how many octaves (floating point) we need to go up to reach C_ref_freq
const float C_octave_to_ref = log(C_ref_freq/C_base_freq)/log(2.0);
// convert real C_octave_to_ref into octave integer and cents tuning
// now shift by (C_voice_addr_bits+C_pa_data_bits)
const int C_shift_octave = (int)(floor(C_octave_to_ref))+C_voice_addr_bits+C_pa_data_bits-C_ref_octave;
const float C_tuning_cents = C_cents_per_octave*(C_octave_to_ref-floor(C_octave_to_ref));
// calculate tuned frequencies (phase advances per clock)
void freq_init(int transpose)
{
  int i;
  for(i = 0; i < (1 << C_voice_addr_bits); i++)
  {
    int octave = i / C_tones_per_octave;
    int meantone = i % C_tones_per_octave;
    int j = (i - transpose) % (1 << C_voice_addr_bits);
    *voice = j;
    freq[j] = pow(2.0, C_shift_octave+octave+(C_temperament[meantone]+C_tuning_cents)/C_cents_per_octave)+0.5;
    *pitch = freq[j];
  }
}

uint64_t db_sine1x    = 0x800000000L; // key + 0
uint64_t db_sine3x    = 0x080000000L; // key + 19
uint64_t db_sine2x    = 0x008000000L; // key + 12
uint64_t db_sine4x    = 0x000800000L; // key + 24
uint64_t db_sine6x    = 0x000080000L; // key + 31
uint64_t db_sine8x    = 0x000008000L; // key + 36
uint64_t db_sine10x   = 0x000000800L; // key + 40
uint64_t db_sine12x   = 0x000000080L; // key + 43
uint64_t db_sine16x   = 0x000000008L; // key + 48

uint64_t db_rockorgan = 0x888000000L;
uint64_t db_metalorgan= 0x875050000L;
uint64_t db_brojack   = 0x800000888L;
uint64_t db_vocalist  = 0x784300000L;
uint64_t db_childintime_upper = 0x784300000; // really 0x777000000 or vocalist is better?
uint64_t db_childintime_lower = 0x745201000L;
uint64_t db_starwars_upper = 0x811100000L;
uint64_t db_starwars_lower = 0x800140000L;

uint64_t reg_upper = db_childintime_upper;
uint64_t reg_lower = db_childintime_lower;

//  0 keys - 1x frequency
// 19 keys - 3x frequency
// 12 keys - 2x frequency
// 24 keys - 4x frequency
// 31 keys - 6x frequency
// 36 keys - 8x frequency
// 40 keys - 10x frequency
// 43 keys - 12x frequency
// 48 keys - 16x frequency
const int drawbar_count = 9;
uint8_t drawbar_voice[9] = {0,19,12,24,31,36,40,43,48}; // voice offset for drawbars

int key_volume = 1; // key volume 1-7
uint8_t last_pitch = 0; // last note played, for the pitch bend


// key press: set of voice volumes according to the registration
void key(uint8_t key, int16_t vol, int16_t bend, uint8_t apply, uint64_t registration)
{
  int i;
  uint64_t r = registration;
  uint8_t db_val;
  int16_t db_volume;
  int8_t v; // voice number
  uint8_t a; // voice address uint
  for(i = drawbar_count-1; i >= 0; i--)
  {
    db_val = r & 15;
    r >>= 4;
    v = key+drawbar_voice[i];
    if(v >= 0)
    {
      a = v;
      db_volume = (1 << db_val)/2;
      volume[a] += vol * db_volume;
      if(apply)
      {
       *voice = a | (volume[a] << 8);
       *pitch = freq[a] + 10*bend;
      }
    }
  }
}


// -----------------------------------------------------------------------------

// This function will be automatically called when a NoteOn is received.
// It must be a void-returning function with the correct parameters,
// see documentation here:
// http://arduinomidilib.fortyseveneffects.com/a00022.html

void handleNoteOn(byte channel, byte pitch, byte velocity)
{
    //if(channel == 1)
    {
      key(pitch, key_volume, 0, 1, pitch < 60 ? reg_lower : reg_upper);
      led_value ^= pitch;
      *led_indicator_pointer = led_value;
      last_pitch = pitch; // for pitch bend
    }
}

void handleNoteOff(byte channel, byte pitch, byte velocity)
{
    //if(channel == 1)
    {
      key(pitch, -key_volume, 0, 1, pitch < 60 ? reg_lower : reg_upper);
      led_value ^= pitch;
      *led_indicator_pointer = led_value;
    }
}

void handlePitchBend(byte channel, int bend)
{
  key(last_pitch, 0, bend, 1, last_pitch < 60 ? reg_lower : reg_upper);
  #if 0
  bend_float = (float) bend;              // Convert bend (int) to float type to get also the after zero numbers
  float bendfactor = (1+ bend_float/8190);      // Calculate the bend factor, with wich the tone() in [hz] shall be bended. Bendfactor shall be between 0.1 and 2, if no bending is applied --> 1.
  if ((freq*bendfactor) > 50)                 // To prevent the output from beeing in an unstable state when note is not properly ended (e.g. not "OFF" has been send
      tone(tonePin,freq*bendfactor);              // Write pitchbended tone to the output. E.g. bend: 8192: 440Hz * 2  --> 880hz --> one Octave higher
  #endif
}

// -----------------------------------------------------------------------------

void setup()
{
    // F32C specific by default is disabled but just in case...
    // Serial.setXoffXon(FALSE);
    // Connect the handleNoteOn function to the library,
    // so it is called upon reception of a NoteOn.
    MIDI.setHandleNoteOn(handleNoteOn);  // Put only the name of the function

    // Do the same for NoteOffs
    MIDI.setHandleNoteOff(handleNoteOff);

    // Handle the Pitch Bend
    MIDI.setHandlePitchBend(handlePitchBend);

    // Initiate MIDI communications, listen to all channels
    MIDI.begin(MIDI_CHANNEL_OMNI);

    #if 0
    *voice = 69 | (1000<<8);
    *pitch = 3000000;
    delay(500);
    *pitch = 4000000;
    delay(500);
    *voice = 69;
    #endif

    freq_init(0);
    led_indicator_pointer = portOutputRegister(digitalPinToPort(led_indicator_pin));
    #if 1
    key(69,7,0,1,db_sine1x);
    *led_indicator_pointer = 255;
    delay(500);
    *led_indicator_pointer = 0;
    key(69,-7,0,1,db_sine1x);
    #endif

    #if 0
    // key click test
    int i;
    for(i = 0; i < 40; i++)
    {
      key(75,6,0,1,db_sine1x);
      delay(100);
      key(75,-6,0,1,-db_sine1x);
      delay(10);
    }
    #endif
}

void loop()
{
    // Call MIDI.read the fastest you can for real-time performance.
    MIDI.read();

    // There is no need to check if there are messages incoming
    // if they are bound to a Callback function.
    // The attached method will be called automatically
    // when the corresponding message has been received.
}

