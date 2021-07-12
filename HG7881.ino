/*
	HG7881.ino

	Wristwatch rewinder box driver program running on an Arduino Nano.

	The machine is using two pair of parallel connected motors driven by a
	HG7881 module. The unit has two input switches. Both of them has 5 states:
	Off, Left, Right, Alternate1, Alternate2.

	Wiring of Arduino pins:
	Analog input pins A0-A7 are connected to the selector switches. A0-A6 are
	pulled up to 5V by internal resistors. Since A6 and A7 does not have these,
	they are pulled up to 5V by external 100k resistors.
	A0-A3 are connected to the left and A4-A7 to the right switch pins. The
	common pins of the switches are connected to each other, and grounded
	through a 470 Ohm resistor.

	The HG7881 motor driver module input pins are connected to the following
	Arduino pins:
	D3:   A-IB
	D11:  A-IA
	D9:   B-IB
	D10:  B-IA
 	The 3V motors are powered by about a 60% duty cycle PWM value of the 5V
	supply voltage.

	The motors are operated in 30min cycles.
	Left/Right mode: [30s run; 30s stop] repeated 12 times, then 18min stop.
	Alt1 mode: [30s run; 30s stop; reverse] repeated 12 times, then 18min stop.
	Alt2 mode: [30s run; 30s stop; reverse] repeated 6 times, then 24min stop.

	Miklos Vegh, Jan 2021.
*/

// Output pin assignment
#define A_IB	3	// D3  -> A_IB
#define A_IA	11	// D11 -> A_IA

#define B_IB	9	// D9  -> B_IB
#define B_IA	10	// D10 -> B-IA

#define LED		13	// D13 -> Internal LED

// Motor speed. The PWM control value is calculated as: speed = 3V / 5V * 255
// which gives a value of 153.
#define MAX_SPEED 150

// Motors are started and stopped by gradually changing the speed. Speed is
// controlled by PWM duty cycle. The speed is adjusted in 100us steps.
#define Time_Update 100

// Timer values for the running, pause, wait states
#define Time_Run		(30 * 1000UL)		// 30s
#define Time_Pause		(30 * 1000UL)		// 30s
#define Time_Wait_LR	(18 * 60 * 1000UL)	// 18 minutes (Left/Right state)
#define Time_Wait_Alt1	(18 * 60 * 1000UL)	// 18 minutes (Alt1 state)
#define Time_Wait_Alt2	(27 * 60 * 1000UL)	// 27 minutes (Alt2 state)

// Cycle repeat count for states
#define Repeat_LRA1		12
#define Repeat_A2		6

// Motor and switch location of the unit.
enum Location { LeftSide, RightSide };

bool testMode = false; // In test mode the timer values are small.


// === SPrint ==================================================================
// Print status info on the serial line.

void SPrint(String s, bool eol = true)
{
	if (Serial) {
		if (eol) {
			Serial.println(s.c_str());
		} else {
			Serial.print(s.c_str());
		}
	}
}


// === class Switch ============================================================
// This class represents the hardware switches of the unit.

class Switch
{
public:
	enum State { Alt2 = 0, Alt1 = 1, Left = 2, Right = 3, Off = 4 };

private:
	State state;
	Location loc;
	byte port[4];

public:
	Switch(Location loc):
		state(Off),
		loc(loc)
	{
		if (loc == LeftSide) {
			port[Alt2]  = A0;
			port[Alt1]  = A1;
			port[Left]	= A2;
			port[Right]	= A3;
		} else {
			port[Alt2]  = A4;
			port[Alt1]  = A5;
			port[Left]	= A6;
			port[Right]	= A7;
		}
	}

	void Update()
	{
		state = Off;

		for (byte i = Alt2; i < Off; ++i) {
			if (analogRead(port[i]) < 500) {
				state = (State) i;
				break;
			}
		}
	}

	State GetState()
	{
		return state;
	}

	String GetStateString()
	{
		String s;
		switch(state) {
			case Alt2:	s = "Alternate Mode 2";	break;
 			case Alt1:	s = "Alternate Mode 1";	break;
 			case Left:	s = "Left";				break;
 			case Right:	s = "Right";			break;
 			case Off:	s = "Off";				break;
			default:	s = "?";				break;
		}
		return s;
	}
};


// === class Motor =============================================================

class Motor
{
public:
	enum Direction { LeftDir, RightDir };

private:
	byte lineA;
	byte lineB;
	short speed;
	Direction dir;

public:
	Motor(Location loc):
		speed(0),
		dir(LeftDir)
	{
		if (loc == LeftSide) {
			lineA = B_IA;
			lineB = B_IB;
		} else {
			lineA = A_IA;
			lineB = A_IB;
		}
		analogWrite(lineA, 0);
		analogWrite(lineB, 0);
	}

	void SetDir(Direction d)
	{
		dir = d;
	}

	void SetSpeed(short s)
	{
		speed = s;
		if (dir == LeftDir) {
			analogWrite(lineA, 0);
			analogWrite(lineB, speed & 0xFF);
		} else {
			analogWrite(lineA, speed & 0xFF);
			analogWrite(lineB, 0);
		}
	}

	byte GetSpeed()
	{
		return (byte) speed;
	}

	void SpeedUp()
	{
		if (speed < MAX_SPEED) {
			speed += 10;
		}
		if (speed > MAX_SPEED) {
			speed = MAX_SPEED;
		}
		SetSpeed(speed);
	}

	void SpeedDown()
	{
		if (speed > 0) {
			speed -= 10;
		}
		if (speed < 0) {
			speed = 0;
		}
		SetSpeed(speed);
	}
};


// === class MotorController ===================================================
// This is a kind of a state machine scheduling the state changes.

class MotorController
{
private:
	enum State { State_Off, State_Right, State_Left, State_Alt1, State_Alt2 };
	enum RunMode { Mode_RunLeft, Mode_RunRight, Mode_Pause, Mode_Wait,
				   Mode_Init };
	enum MotorState { Halt, Run };

	State state;
	RunMode runMode;
	Switch sw;
	Motor motor;
	Motor::Direction dir;
	unsigned long t0; // Time elapsed since the last state change
	unsigned long tUpdate; // Time elapsed since the last update
	short repeatCounter;

	void ChangeState()
	{
		switch (sw.GetState()) {
			case Switch::Right:
				state = State_Right;
				dir = Motor::RightDir;
				runMode = Mode_Init;
				break;

			case Switch::Left:
				state = State_Left;
				dir = Motor::LeftDir;
				runMode = Mode_Init;
				break;

			case Switch::Alt1:
				state = State_Alt1;
				dir = Motor::LeftDir;
				runMode = Mode_RunLeft;
				runMode = Mode_Init;
				break;

			case Switch::Alt2:
				state = State_Alt2;
				dir = Motor::LeftDir;
				runMode = Mode_RunLeft;
				runMode = Mode_Init;
				break;

			case Switch::Off:
				state = State_Off;
				dir = Motor::LeftDir;
				runMode = Mode_Init;
				break;

			default:
				break;
		}
		t0 = millis();
	}

	void AdjustMotorState(MotorState mState)
	{
		if (mState == Run) {
			if (motor.GetSpeed() < MAX_SPEED) {
				motor.SetDir(dir);
				motor.SpeedUp();
			}
		} else {
			if (motor.GetSpeed() > 0) {
				motor.SpeedDown();
			}
		}
	}

	unsigned long GetTimeLimit(State s, RunMode m)
	{
		unsigned long t;
		switch(m) {
			case Mode_RunLeft:
			case Mode_RunRight:
				t = testMode ? Time_Run / 10 : Time_Run;
				break;

			case Mode_Pause:
				t = testMode ? Time_Pause / 10 : Time_Pause;
				break;

			case Mode_Wait:
			default:
				if (s == State_Alt1) {
					t = Time_Wait_Alt1;
				} else if (s == State_Alt2) {
					t = Time_Wait_Alt2;
				} else {
					t = Time_Wait_LR;
				}
				if (testMode) {
					t = t / 40;
				}
				break;
		}
		return t;
	}

	short GetRepeatCounter(State s)
	{
		return s == State_Alt2 ? Repeat_A2 : Repeat_LRA1;
	}

	void UpdateRightState()
	{
		if (sw.GetState() == Switch::Right) {
			// Switch is already in the 'Right' position.
			// Set motor speed and direction according to the current runMode.
			if (runMode == Mode_RunRight) {
				if (dir == Motor::RightDir) {
					// Direction is ok, spin up & run
					AdjustMotorState(Run);
				} else {
					// Direction is opposite as needed: slow down & reverse
					if (motor.GetSpeed() != 0) {
						AdjustMotorState(Halt);
					} else {
						dir = Motor::RightDir;
					}
				}
			} else { // Halt in all other modes
				AdjustMotorState(Halt);
			}

			// Update timer
			unsigned long time = millis();
			if (time < t0) { // Check for counter overflow
				t0 = time;
			}

			// Adjust runMode
			switch(runMode) {
				case Mode_RunRight:
					if (time - t0 > GetTimeLimit(State_Right, runMode)) {
						t0 = time;
						runMode = Mode_Pause;
					}
					break;

				case Mode_Pause:
					if (time - t0 > GetTimeLimit(State_Right, runMode)) {
						t0 = time;
						if (repeatCounter <= 0) {
							repeatCounter = GetRepeatCounter(State_Right);
							runMode = Mode_Wait;
						} else {
							--repeatCounter;
							runMode = Mode_RunRight;
						}
					}
					break;

				case Mode_Wait:
					if (time - t0 > GetTimeLimit(State_Right, runMode)) {
						t0 = time;
						runMode = Mode_RunRight;
					}
					break;

				case Mode_Init:
				default:
					// State changed
					t0 = time;
					runMode = Mode_RunRight;
					repeatCounter = GetRepeatCounter(State_Right);
					break;
			}
		} else {
			// Change machine state due to switch change
			if (motor.GetSpeed() > 0) {
				AdjustMotorState(Halt);
			} else {
				ChangeState();
			}
		}
	}

	void UpdateLeftState()
	{
		if (sw.GetState() == Switch::Left) {
			// Switch is already in the 'Left' position.
			// Set motor speed and direction according to the current runMode.
			if (runMode == Mode_RunLeft) {
				if (dir == Motor::LeftDir) {
					// Direction is ok, spin up & run
					AdjustMotorState(Run);
				} else {
					// Direction is opposite as needed: slow down & reverse
					if (motor.GetSpeed() != 0) {
						AdjustMotorState(Halt);
					} else {
						dir = Motor::LeftDir;
					}
				}
			} else { // Halt in all other modes
				AdjustMotorState(Halt);
			}

			// Update timer
			unsigned long time = millis();
			if (time < t0) { // Check for counter overflow
				t0 = time;
			}

			// Adjust runMode
			switch(runMode) {
				case Mode_RunLeft:
					if (time - t0 > GetTimeLimit(State_Left, runMode)) {
						t0 = time;
						runMode = Mode_Pause;
					}
					break;

				case Mode_Pause:
					if (time - t0 > GetTimeLimit(State_Left, runMode)) {
						t0 = time;
						if (repeatCounter <= 0) {
							repeatCounter = GetRepeatCounter(State_Left);
							runMode = Mode_Wait;
						} else {
							--repeatCounter;
							runMode = Mode_RunLeft;
						}
					}
					break;

				case Mode_Wait:
					if (time - t0 > GetTimeLimit(State_Left, runMode)) {
						t0 = time;
						runMode = Mode_RunLeft;
					}
					break;

				case Mode_Init:
				default:
					// State changed
					t0 = time;
					runMode = Mode_RunLeft;
					repeatCounter = GetRepeatCounter(State_Left);
					break;
			}
		} else {
			// Change machine state due to switch change
			if (motor.GetSpeed() > 0) {
				AdjustMotorState(Halt);
			} else {
				ChangeState();
			}
		}
	}

	void UpdateAltState()
	{
		if ((sw.GetState() == Switch::Alt1 && state == State_Alt1) ||
			(sw.GetState() == Switch::Alt2 && state == State_Alt2)) {
			// Switch is already in the 'Alt1' position.
			// Set motor speed and direction according to the current runMode.
			if (runMode == Mode_RunLeft) {
				if (dir == Motor::LeftDir) {
					// Direction is ok, spin up & run
					AdjustMotorState(Run);
				} else {
					// Direction is opposite as needed: slow down & reverse
					if (motor.GetSpeed() != 0) {
						AdjustMotorState(Halt);
					} else {
						dir = Motor::LeftDir;
					}
				}
			} else if (runMode == Mode_RunRight) {
				if (dir == Motor::RightDir) {
					// Direction is ok, spin up & run
					AdjustMotorState(Run);
				} else {
					// Direction is opposite as needed: slow down & reverse
					if (motor.GetSpeed() != 0) {
						AdjustMotorState(Halt);
					} else {
						dir = Motor::RightDir;
					}
				}
			} else { // Mode_Pause, Mode_Wait
				// Halt in all other modes
				AdjustMotorState(Halt);
			}

			// Update timer
			unsigned long time = millis();
			if (time < t0) { // Check for counter overflow
				t0 = time;
			}

			// Adjust runMode
			switch(runMode) {
				case Mode_RunLeft:
				case Mode_RunRight:
					if (time - t0 > GetTimeLimit(state, runMode)) {
						t0 = time;
						runMode = Mode_Pause;
					}
					break;

				case Mode_Pause:
					if (time - t0 > GetTimeLimit(state, runMode)) {
						t0 = time;
						if (repeatCounter <= 0) {
							repeatCounter = GetRepeatCounter(state);
							runMode = Mode_Wait;
						} else {
							// Switch direction
							if (dir == Motor::LeftDir) {
								runMode = Mode_RunRight;
								--repeatCounter;
							} else {
								runMode = Mode_RunLeft;
							}
						}
					}
					break;

				case Mode_Wait:
					if (time - t0 > GetTimeLimit(state, runMode)) {
						t0 = time;
						runMode = Mode_RunLeft;
					}
					break;

				default:
					// State changed
					t0 = time;
					runMode = Mode_RunLeft;
					repeatCounter = GetRepeatCounter(state);
					break;
			}
		} else {
			// Change machine state due to switch change
			if (motor.GetSpeed() > 0) {
				AdjustMotorState(Halt);
			} else {
				ChangeState();
			}
		}
	}

public:
	MotorController(Location loc):
		state(State_Off),
		runMode(Mode_RunLeft),
		sw(loc),
		motor(loc),
		dir(Motor::LeftDir),
		t0(millis()),
		tUpdate(millis()),
		repeatCounter(0)
	{
	}

	void Update()
	{
		unsigned long time = millis();
		if (tUpdate > time) { // Check for overflow
			tUpdate = time;
		}
		if (time - tUpdate < Time_Update) {
			return;
		}
		tUpdate = time;

		sw.Update();

		switch (state) {
			case State_Right:
				UpdateRightState();
				break;

			case State_Left:
				UpdateLeftState();
				break;

			case State_Alt1:
			case State_Alt2:
				UpdateAltState();
				break;

			case State_Off:
				if (motor.GetSpeed() > 0) {
					AdjustMotorState(Halt);
				} else if (sw.GetState() != Switch::Off) {
					ChangeState();
				}
				break;

			default:
				break;
		}
	}

	Switch::State GetSwitchState()
	{
		return sw.GetState();
	}

	String GetSwitchStateString()
	{
		return sw.GetStateString();
	}
};


// === Led =====================================================================
// The internal led is turned on and off for 30 ms four times in one second

class Led
{
	unsigned long t0;

public:
	Led():
		t0(millis())
	{
		digitalWrite(LED, 0);
	}

	void Update()
	{
		unsigned long time = millis();
		if (t0 > time) { // Check for overflow
			t0 = time;
		}
		unsigned long dt = time - t0;
		if (dt < 1000) {
			for (unsigned short i = 1; i <= 8; ++i) {
				if (dt < i * 30) {
					digitalWrite(LED, i % 2);
					break;
				}
			}
		} else {
			digitalWrite(LED, 0);
			t0 = time;
		}
	}
};


// === Rewinder ================================================================

class Machine
{
private:
	MotorController mc1;
	MotorController mc2;
	Switch::State sw1;
	Switch::State sw2;

public:
	Machine():
		mc1(LeftSide),
		mc2(RightSide)
	{
		sw1 = mc1.GetSwitchState();
		sw2 = mc2.GetSwitchState();
	}

	void Update()
	{
		mc1.Update();
		mc2.Update();

		Switch::State s1 = mc1.GetSwitchState();
		Switch::State s2 = mc2.GetSwitchState();
		if (s1 != sw1 || s2 != sw2) {
			// Print switch states if they were changed
			sw1 = s1;
			sw2 = s2;
			PrintState();
		}
	}

	void PrintState()
	{
		String s = "Left switch: [" + mc2.GetSwitchStateString() +
				   "], Right switch: [" + mc1.GetSwitchStateString() + "], " +
				   "Test Mode: " + (testMode ? "ON" : "OFF") + ".";
		SPrint(s.c_str());
	}
};


// === Object instances ========================================================

Machine machine;
Led led;


// === setup () ================================================================

void setup()
{
	pinMode(LED_BUILTIN, OUTPUT);

	pinMode(A0, INPUT_PULLUP);
	pinMode(A1, INPUT_PULLUP);
	pinMode(A2, INPUT_PULLUP);
	pinMode(A3, INPUT_PULLUP);
	pinMode(A4, INPUT_PULLUP);
	pinMode(A5, INPUT_PULLUP);
	pinMode(A6, INPUT);	// External pullup used
	pinMode(A7, INPUT);	// External pullup used

	pinMode(A_IB, OUTPUT);
	pinMode(A_IA, OUTPUT);

	pinMode(B_IB, OUTPUT);
	pinMode(B_IA, OUTPUT);

	pinMode(LED, OUTPUT);

	Serial.begin(9600);
	delay(500);

	// Set PWM timers to the max frequency to eliminate audible PWM noise.
	// Pin 9 and 10 are controlled by timer 1
	TCCR1B = (TCCR1B & 0b11111000) | 0x01;
	// Pin 3 and 11 are controlled by timer 2
	TCCR2B = (TCCR2B & 0b11111000) | 0x01;

	SPrint("+------------------------------------------------------+");
	SPrint("| ---=== Wristwatch Rewinder Machine Controller ===--- |");
	SPrint("|      Built on Arduino Nano and a HG7881 module.      |");
	SPrint("|                                        Version: 1.0. |");
	SPrint("| Constructed & developed:  Miklos Vegh, January 2021. |");
	SPrint("+------------------------------------------------------+");
	SPrint("| Commands:                                            |");
	SPrint("| 'N': Enter Normal Mode                               |");
	SPrint("| 'T': Enter Test Mode                                 |");
	SPrint("| '?': Print Machine State                             |");
	SPrint("+------------------------------------------------------+");
	SPrint("");
}


// === loop() ==================================================================

void loop()
{
	led.Update();
	machine.Update();

	if (Serial.available()) {
		char c = Serial.read();
		if (c == 'n' || c == 'N') {
			SPrint("Normal Mode selected.");
			testMode = false;
		} else if (c == 't' || c == 'T') {
			SPrint("Test Mode selected.");
			testMode = true;
		} else if (c == '?') {
			machine.PrintState();
		}
	}

	delay(5);
}
