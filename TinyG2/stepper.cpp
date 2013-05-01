/*
 * stepper.cpp - stepper motor controls
 * This file is part of the TinyG project
 *
 * Copyright (c) 2013 Alden S. Hart Jr.
 * Copyright (c) 2013 Robert Giseburt
 *
 * This file ("the software") is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2 as published by the
 * Free Software Foundation. You should have received a copy of the GNU General Public
 * License, version 2 along with the software.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, you may use this file as part of a software library without
 * restriction. Specifically, if other files instantiate templates or use macros or
 * inline functions from this file, or you compile this file and link it with  other
 * files to produce an executable, this file does not by itself cause the resulting
 * executable to be covered by the GNU General Public License. This exception does not
 * however invalidate any other reasons why the executable file might be covered by the
 * GNU General Public License.
 *
 * THE SOFTWARE IS DISTRIBUTED IN THE HOPE THAT IT WILL BE USEFUL, BUT WITHOUT ANY
 * WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
 * SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
/* 	This module provides the low-level stepper drivers and some related
 * 	functions. It dequeues lines queued by the motor_queue routines.
 * 	This is some of the most heavily optimized code in the project.
 *	Please refer to the end of the stepper.h file for a more complete explanation
 */
#include "tinyg2.h"
#include "config.h"
#include "hardware.h"
#include "planner.h"
#include "stepper.h"
//#include "motatePins.h"		// defined in hardware.h   Not needed here
#include "motateTimers.h"
#include "util.h"

//#define TEST_CODE

using namespace Motate;

// Setup local resources
Motate::Timer<dda_timer_num> dda_timer;			// stepper pulse generation
Motate::Timer<dwell_timer_num> dwell_timer;		// dwell timer
Motate::Timer<load_timer_num> load_timer;		// triggers load of next stepper segment
Motate::Timer<exec_timer_num> exec_timer;		// triggers calculation of next+1 stepper segment
Motate::OutputPin<31> proof_of_timer;

// Setup a stepper template to hold our pins
template<pin_number step_num, pin_number dir_num, pin_number enable_num, 
		 pin_number ms0_num, pin_number ms1_num, pin_number vref_num>

struct Stepper {
	OutputPin<step_num> step;
	OutputPin<dir_num> dir;
	OutputPin<enable_num> enable;
	OutputPin<ms0_num> ms0;
	OutputPin<ms1_num> ms1;
	OutputPin<vref_num> vref;
};
Stepper<motor_1_step_pin_num, 
		motor_1_dir_pin_num, 
		motor_1_enable_pin_num, 
		motor_1_microstep_0_pin_num, 
		motor_1_microstep_1_pin_num,
		motor_1_vref_pin_num> motor_1;

Stepper<motor_2_step_pin_num, 
		motor_2_dir_pin_num, 
		motor_2_enable_pin_num, 
		motor_2_microstep_0_pin_num, 
		motor_2_microstep_1_pin_num,
		motor_2_vref_pin_num> motor_2;

Stepper<motor_3_step_pin_num, 
		motor_3_dir_pin_num, 
		motor_3_enable_pin_num, 
		motor_3_microstep_0_pin_num, 
		motor_3_microstep_1_pin_num,
		motor_3_vref_pin_num> motor_3;

Stepper<motor_4_step_pin_num, 
		motor_4_dir_pin_num, 
		motor_4_enable_pin_num, 
		motor_4_microstep_0_pin_num, 
		motor_4_microstep_1_pin_num,
		motor_4_vref_pin_num> motor_4;

Stepper<motor_5_step_pin_num, 
		motor_5_dir_pin_num, 
		motor_5_enable_pin_num, 
		motor_5_microstep_0_pin_num, 
		motor_5_microstep_1_pin_num,
		motor_5_vref_pin_num> motor_5;
		
Stepper<motor_6_step_pin_num, 
		motor_6_dir_pin_num, 
		motor_6_enable_pin_num, 
		motor_6_microstep_0_pin_num, 
		motor_6_microstep_1_pin_num,
		motor_6_vref_pin_num> motor_6;

OutputPin<motor_enable_pin_num> enable;

//volatile long dummy;					// convenient register to read into

static void _load_move(void);
static void _exec_move(void);
static void _request_load_move(void);
static void _clear_diagnostic_counters(void);

enum prepBufferState {
	PREP_BUFFER_OWNED_BY_LOADER = 0,	// staging buffer is ready for load
	PREP_BUFFER_OWNED_BY_EXEC			// staging buffer is being loaded
};

#define INCREMENT_DIAGNOSTIC_COUNTER(motor)	// chose this one to disable counters
//#define INCREMENT_DIAGNOSTIC_COUNTER(motor) st.m[motor].step_count_diagnostic++;

/*
 * Stepper structures
 *
 *	There are 4 sets of structures involved in this operation;
 *
 *	data structure:						static to:		runs at:
 *	  mpBuffer planning buffers (bf)	  planner.c		  main loop
 *	  mrRuntimeSingleton (mr)			  planner.c		  MED ISR
 *	  stPrepSingleton (sp)				  stepper.c		  MED ISR
 *	  stRunSingleton (st)				  stepper.c		  HI ISR
 *  
 *	Care has been taken to isolate actions on these structures to the 
 *	execution level in which they run and to use the minimum number of 
 *	volatiles in these structures. This allows the compiler to optimize
 *	the stepper inner-loops better.
 */

// Runtime structure. Used exclusively by step generation ISR (HI)
typedef struct stRunMotor { 			// one per controlled motor
	int32_t phase_increment;			// = total steps in axis times substep factor
	int32_t phase_accumulator;			// DDA phase angle accumulator for axis
	uint32_t step_count_diagnostic;		// diagnostic - comment out for normal use
} stRunMotor_t;

typedef struct stRunSingleton {			// Stepper static values and axis parameters
	uint16_t magic_start;				// magic number to test memory integrity	
	int32_t timer_ticks_downcount;		// tick down-counter (unscaled)
	int32_t timer_ticks_X_substeps;		// ticks multiplied by scaling factor
	stRunMotor_t m[MOTORS];				// runtime motor structures
} stRunSingleton_t;

// Prep-time structure. Used by exec/prep ISR (MED) and read-only during load 
// Must be careful about volatiles in this one

typedef struct stPrepMotor {
 	uint32_t phase_increment; 			// = total steps in axis times substep factor
	int8_t dir;							// direction
} stPrepMotor_t;

typedef struct stPrepSingleton {
	uint16_t magic_start;				// magic number to test memory integrity	
	uint8_t move_type;					// move type
	volatile uint8_t exec_state;		// move execution state 
	volatile uint8_t counter_reset_flag; // set TRUE if counter should be reset
	uint32_t prev_ticks;				// tick count from previous move
	uint32_t timer_ticks;				// DDA or dwell ticks for the move
	uint32_t timer_ticks_X_substeps;	// DDA ticks scaled by substep factor
	stPrepMotor_t m[MOTORS];			// per-motor structures
} stPrepSingleton_t;

// Structure allocation
static stRunSingleton_t st;
static struct stPrepSingleton sps;

magic_t st_get_st_magic() { return (st.magic_start);}
magic_t st_get_sps_magic() { return (sps.magic_start);}

/*
 * stepper_init() - initialize stepper motor subsystem 
 *
 *	Notes:
 *	  - This init requires sys_init() to be run beforehand
 *		This init is a precursor for gpio_init()
 * 	  - microsteps are setup during cfg_init()
 *	  - motor polarity is setup during cfg_init()
 *	  - high level interrupts must be enabled in main() once all inits are complete
 */

void stepper_init()
{
	memset(&st, 0, sizeof(st));			// clear all values, pointers and status
	st.magic_start = MAGICNUM;
	sps.magic_start = MAGICNUM;

	// setup DDA timer
#ifdef BARE_CODE
	//requires: #include <component_tc.h>
	REG_TC1_WPMR = 0x54494D00;			// enable write to registers
	TC_Configure(TC_BLOCK_DDA, TC_CHANNEL_DDA, TC_CMR_DDA);
	REG_RC_DDA = TC_RC_DDA;				// set frequency
	REG_IER_DDA = TC_IER_DDA;			// enable interrupts
	NVIC_EnableIRQ(TC_IRQn_DDA);
	pmc_enable_periph_clk(TC_ID_DDA);
	TC_Start(TC_BLOCK_DDA, TC_CHANNEL_DDA);
#else
	dda_timer.setModeAndFrequency(kTimerUpToMatch, FREQUENCY_DDA);
	dda_timer.setInterrupts(kInterruptOnOverflow | kInterruptPriorityHighest);
#endif

	// setup DWELL timer
	dwell_timer.setModeAndFrequency(kTimerUpToMatch, FREQUENCY_DWELL);
	dwell_timer.setInterrupts(kInterruptOnOverflow | kInterruptPriorityHighest);

	// setup LOAD timer
	load_timer.setModeAndFrequency(kTimerUpToMatch, FREQUENCY_SGI);
	load_timer.setInterrupts(kInterruptOnSoftwareTrigger | kInterruptPriorityLow);

	// setup EXEC timer
	exec_timer.setModeAndFrequency(kTimerUpToMatch, FREQUENCY_SGI);
	exec_timer.setInterrupts(kInterruptOnSoftwareTrigger | kInterruptPriorityLowest);

	sps.exec_state = PREP_BUFFER_OWNED_BY_EXEC;		// initial condition
	_clear_diagnostic_counters();
/*
#ifdef TEST_CODE
	sps.move_type = true;
	sps.timer_ticks = 100000;
	sps.timer_ticks_X_substeps = 1000000;
	
	st.m[MOTOR_1].phase_increment = 90000;
	st.m[MOTOR_1].phase_accumulator = -sps.timer_ticks_X_substeps;
	st.timer_ticks_X_substeps = sps.timer_ticks_X_substeps;

	dda_timer.start();
#endif
*/
}

/*
 * st_enable() - start the steppers
 * st_disable() - step the stoppers
 */
void st_enable()
{
	enable.clear();		// grblShield common enable
	dda_timer.start();
}

void st_disable()
{
	dda_timer.stop();
	enable.set();		// grblShield common enable
	motor_1.enable.set();
	motor_2.enable.set();
	motor_4.enable.set();
	motor_5.enable.set();
	motor_6.enable.set();
	st.m[MOTOR_1].phase_increment = 0;
	st.m[MOTOR_2].phase_increment = 0;
	st.m[MOTOR_3].phase_increment = 0;
	st.m[MOTOR_4].phase_increment = 0;
	st.m[MOTOR_5].phase_increment = 0;
	st.m[MOTOR_6].phase_increment = 0;
}
// clear diagnostic counters
static void _clear_diagnostic_counters()
{
	st.m[MOTOR_1].step_count_diagnostic = 0;
	st.m[MOTOR_2].step_count_diagnostic = 0;
	st.m[MOTOR_3].step_count_diagnostic = 0;
	st.m[MOTOR_4].step_count_diagnostic = 0;
	st.m[MOTOR_5].step_count_diagnostic = 0;
	st.m[MOTOR_6].step_count_diagnostic = 0;
}

// Define the timer interrupts inside the Motate namespace
namespace Motate {

/*
 * Dwell timer interrupt
 */
MOTATE_TIMER_INTERRUPT(dwell_timer_num) 
{
	dwell_timer.getInterruptCause(); // read SR to clear interrupt condition
	if (--st.timer_ticks_downcount == 0) {
		dwell_timer.stop();
		_load_move();
	}
}

/****************************************************************************************
 * ISR - DDA timer interrupt routine - service ticks from DDA timer
 *
 *	Uses direct struct addresses and literal values for hardware devices -
 *	it's faster than using indexed timer and port accesses. I checked.
 *	Even when -0s or -03 is used.
 */
MOTATE_TIMER_INTERRUPT(dda_timer_num)
{
	dda_timer.getInterruptCause(); // read SR to clear interrupt condition
    proof_of_timer = 0;
    
    if (!motor_1.step.isNull() && (st.m[MOTOR_1].phase_accumulator += st.m[MOTOR_1].phase_increment) > 0) {
        st.m[MOTOR_1].phase_accumulator -= st.timer_ticks_X_substeps;
		motor_1.step.set();		// turn step bit on
		INCREMENT_DIAGNOSTIC_COUNTER(MOTOR_1);
    }
    if (!motor_2.step.isNull() && (st.m[MOTOR_2].phase_accumulator += st.m[MOTOR_2].phase_increment) > 0) {
        st.m[MOTOR_2].phase_accumulator -= st.timer_ticks_X_substeps;
        motor_2.step.set();
		INCREMENT_DIAGNOSTIC_COUNTER(MOTOR_2);
    }
    if (!motor_3.step.isNull() && (st.m[MOTOR_3].phase_accumulator += st.m[MOTOR_3].phase_increment) > 0) {
        st.m[MOTOR_3].phase_accumulator -= st.timer_ticks_X_substeps;
        motor_3.step.set();
		INCREMENT_DIAGNOSTIC_COUNTER(MOTOR_3);
    }
    if (!motor_4.step.isNull() && (st.m[MOTOR_4].phase_accumulator += st.m[MOTOR_4].phase_increment) > 0) {
        st.m[MOTOR_4].phase_accumulator -= st.timer_ticks_X_substeps;
        motor_4.step.set();
		INCREMENT_DIAGNOSTIC_COUNTER(MOTOR_4);
    }
    if (!motor_5.step.isNull() && (st.m[MOTOR_5].phase_accumulator += st.m[MOTOR_5].phase_increment) > 0) {
        st.m[MOTOR_5].phase_accumulator -= st.timer_ticks_X_substeps;
        motor_5.step.set();
		INCREMENT_DIAGNOSTIC_COUNTER(MOTOR_5);
    }
    if (!motor_6.step.isNull() && (st.m[MOTOR_6].phase_accumulator += st.m[MOTOR_6].phase_increment) > 0) {
        st.m[MOTOR_6].phase_accumulator -= st.timer_ticks_X_substeps;
        motor_6.step.set();
		INCREMENT_DIAGNOSTIC_COUNTER(MOTOR_6);
    }
    motor_1.step.clear();
    motor_2.step.clear();
    motor_3.step.clear();
    motor_4.step.clear();
    motor_5.step.clear();
    motor_6.step.clear();

    if (--st.timer_ticks_downcount == 0) {			// process end of move
        // power-down motors if this feature is enabled
        if (cfg.m[MOTOR_1].power_mode == true) { motor_1.enable.set(); }
        if (cfg.m[MOTOR_2].power_mode == true) { motor_2.enable.set(); }
        if (cfg.m[MOTOR_3].power_mode == true) { motor_3.enable.set(); }
        if (cfg.m[MOTOR_4].power_mode == true) { motor_4.enable.set(); }
        if (cfg.m[MOTOR_5].power_mode == true) { motor_5.enable.set(); }
        if (cfg.m[MOTOR_6].power_mode == true) { motor_6.enable.set(); }
		st_disable();
        _load_move();								// load the next move
    }
    proof_of_timer = 1;
}

} // namespace Motate

/****************************************************************************************
 * Exec sequencing code
 *
 * st_request_exec_move()	- SW interrupt to request to execute a move
 * exec_timer interrupt		- interrupt handler for calling exec function
 * _exec_move() 			- Run a move from the planner and prepare it for loading
 *
 *	_exec_move() should only be called be called from an ISR at a level lower than DDA.
 *	Only use st_request_exec_move() to call it.
 */
void st_request_exec_move()
{
	if (sps.exec_state == PREP_BUFFER_OWNED_BY_EXEC) {	// bother interrupting
		exec_timer.setInterruptPending();
	}
}

// Define the timers inside the Motate namespace
namespace Motate {

MOTATE_TIMER_INTERRUPT(exec_timer_num)			// exec move SW interrupt
{
	exec_timer.getInterruptCause();				// read SR to clear interrupt condition
	_exec_move();
}

} // namespace Motate
/*
static void _exec_move()
{
   	if (sps.exec_state == PREP_BUFFER_OWNED_BY_EXEC) {
		if (mp_exec_move() != STAT_NOOP) {
			sps.exec_state = PREP_BUFFER_OWNED_BY_LOADER; // flip it back
			_request_load_move();
		}
	}
}
*/
static void _exec_move()
{
	if (sps.exec_state == PREP_BUFFER_OWNED_BY_EXEC) {
		if (mp_exec_move() != STAT_NOOP) {
			sps.exec_state = PREP_BUFFER_OWNED_BY_LOADER; // flip it back
			_request_load_move();
		} else {
			st_prep_null();
		}
	}
}

/****************************************************************************************
 * Load sequencing code
 *
 * _request_load()		- fires a software interrupt (timer) to request to load a move
 *  load_mode interrupt	- interrupt handler for running the loader
 * _load_move() 		- load a move into steppers, load a dwell, or process a Null move
 */

static void _request_load_move()
{
	if (st.timer_ticks_downcount == 0) {	// bother interrupting
		load_timer.setInterruptPending();
	} 	// ...else don't bother to interrupt. 
		// You'll just trigger an interrupt and find out the loader is not ready
}

// Define the timers inside the Motate namespace
namespace Motate {

MOTATE_TIMER_INTERRUPT(load_timer_num)		// load steppers SW interrupt
{
	load_timer.getInterruptCause();			// read SR to clear interrupt condition
	_load_move();
}
} // namespace Motate

/*
 * _load_move() - Dequeue move and load into stepper struct
 *
 *	This routine can only be called be called from an ISR at the same or 
 *	higher level as the DDA or dwell ISR. A software interrupt has been 
 *	provided to allow a non-ISR to request a load (see st_request_load_move())
 *
 *	In aline code:
 *	 - All axes must set steps and compensate for out-of-range pulse phasing.
 *	 - If axis has 0 steps the direction setting can be omitted
 *	 - If axis has 0 steps the motor must not be enabled to support power mode = 1
 */

void _load_move()
{
/*
#ifdef TEST_CODE
    sps.move_type = true;

	sps.timer_ticks = 100000;
	sps.timer_ticks_X_substeps = 1000000;
	st.m[MOTOR_1].phase_increment = 90000;
	st.m[MOTOR_1].phase_accumulator = -sps.timer_ticks;
	st.timer_ticks_X_substeps = sps.timer_ticks_X_substeps;
	st_enable(); 
//    dda_timer.start();
    return;
#endif
*/
	// handle aline loads first (most common case)  NB: there are no more lines, only alines
	if (sps.move_type == MOVE_TYPE_ALINE) {
		st.timer_ticks_downcount = sps.timer_ticks;
		st.timer_ticks_X_substeps = sps.timer_ticks_X_substeps;
 
		st.m[MOTOR_1].phase_increment = sps.m[MOTOR_1].phase_increment;
		if (sps.counter_reset_flag == true) {	// compensate for pulse phasing
			st.m[MOTOR_1].phase_accumulator = -(st.timer_ticks_downcount);
		}
		if (st.m[MOTOR_1].phase_increment != 0) {
			if (sps.m[MOTOR_1].dir == 0) {
				motor_1.dir.clear();			// clear bit for clockwise motion 
			} else {
				motor_1.dir.set();				// CCW motion
			}
			motor_1.enable.clear();				// enable motor
		}

		st.m[MOTOR_2].phase_increment = sps.m[MOTOR_2].phase_increment;
		if (sps.counter_reset_flag == true) {
			st.m[MOTOR_2].phase_accumulator = -(st.timer_ticks_downcount);
		}
		if (st.m[MOTOR_2].phase_increment != 0) {
			if (sps.m[MOTOR_2].dir == 0) motor_2.dir.clear(); 
			else motor_2.dir.set();
			motor_2.enable.clear();
		}

		st.m[MOTOR_3].phase_increment = sps.m[MOTOR_3].phase_increment;
		if (sps.counter_reset_flag == true) {
			st.m[MOTOR_3].phase_accumulator = -(st.timer_ticks_downcount);
		}
		if (st.m[MOTOR_3].phase_increment != 0) {
			if (sps.m[MOTOR_3].dir == 0) motor_3.dir.clear();
			else motor_3.dir.set();
			motor_3.enable.clear();
		}

		st.m[MOTOR_4].phase_increment = sps.m[MOTOR_4].phase_increment;
		if (sps.counter_reset_flag == true) {
			st.m[MOTOR_4].phase_accumulator = (st.timer_ticks_downcount);
		}
		if (st.m[MOTOR_4].phase_increment != 0) {
			if (sps.m[MOTOR_4].dir == 0) motor_4.dir.clear();
			else motor_4.dir.set();
			motor_4.enable.clear();
		}

		st.m[MOTOR_5].phase_increment = sps.m[MOTOR_5].phase_increment;
		if (sps.counter_reset_flag == true) {
			st.m[MOTOR_5].phase_accumulator = (st.timer_ticks_downcount);
		}
		if (st.m[MOTOR_5].phase_increment != 0) {
			if (sps.m[MOTOR_5].dir == 0) motor_5.dir.clear();
			else motor_5.dir.set();
			motor_5.enable.clear();
		}

		st.m[MOTOR_6].phase_increment = sps.m[MOTOR_6].phase_increment;
		if (sps.counter_reset_flag == true) {
			st.m[MOTOR_6].phase_accumulator = (st.timer_ticks_downcount);
		}
		if (st.m[MOTOR_6].phase_increment != 0) {
			if (sps.m[MOTOR_6].dir == 0) motor_6.dir.clear();
			else motor_6.dir.set();
			motor_6.enable.clear();
		}
		st_enable();

	// handle dwells
	} else if (sps.move_type == MOVE_TYPE_DWELL) {
		st.timer_ticks_downcount = sps.timer_ticks;
		dwell_timer.start();
	}

	// all other cases drop to here - such as Null moves queued by Mcodes 
	sps.exec_state = PREP_BUFFER_OWNED_BY_EXEC;			// flip it back
	st_request_exec_move();								// exec and prep next move
}

/****************************************************************************************
 * st_prep_line() - Prepare the next move for the loader
 *
 *	This function does the math on the next pulse segment and gets it ready for 
 *	the loader. It deals with all the DDA optimizations and timer setups so that
 *	loading can be performed as rapidly as possible. It works in joint space 
 *	(motors) and it works in steps, not length units. All args are provided as 
 *	floats and converted to their appropriate integer types for the loader. 
 *
 * Args:
 *	steps[] are signed relative motion in steps (can be non-integer values)
 *	Microseconds - how many microseconds the segment should run 
 */

uint8_t st_prep_line(float steps[], float microseconds)
{
	// *** defensive programming ***
	// trap conditions that would prevent queuing the line
	if (sps.exec_state != PREP_BUFFER_OWNED_BY_EXEC) { return (STAT_INTERNAL_ERROR);
	} else if (isfinite(microseconds) == false) { return (STAT_ZERO_LENGTH_MOVE);
	} else if (microseconds < EPSILON) { return (STAT_ZERO_LENGTH_MOVE);
	}
	sps.counter_reset_flag = false;		// initialize counter reset flag for this move.

	// setup motor parameters
	for (uint8_t i=0; i<MOTORS; i++) {
		sps.m[i].dir = ((steps[i] < 0) ? 1 : 0) ^ cfg.m[i].polarity;
		sps.m[i].phase_increment = (uint32_t)fabs(steps[i] * DDA_SUBSTEPS);
	}
	sps.timer_ticks = (uint32_t)((microseconds/1000000) * FREQUENCY_DDA);
	sps.timer_ticks_X_substeps = sps.timer_ticks * DDA_SUBSTEPS;	// see FOOTNOTE

	// anti-stall measure in case change in velocity between segments is too great 
	if ((sps.timer_ticks * COUNTER_RESET_FACTOR) < sps.prev_ticks) {  // NB: uint32_t math
		sps.counter_reset_flag = true;
	}
	sps.prev_ticks = sps.timer_ticks;
	sps.move_type = MOVE_TYPE_ALINE;
	return (STAT_OK);
}
// FOOTNOTE: This expression was previously computed as below but floating 
// point rounding errors caused subtle and nasty accumulated position errors:
// sp.timer_ticks_X_substeps = (uint32_t)((microseconds/1000000) * f_dda * dda_substeps);

/* 
 * st_prep_null() - Keeps the loader happy. Otherwise performs no action
 *
 *	Used by M codes, tool and spindle changes
 */
void st_prep_null()
{
	sps.move_type = MOVE_TYPE_NULL;
}

/* 
 * st_prep_dwell() 	 - Add a dwell to the move buffer
 */

void st_prep_dwell(float microseconds)
{
	sps.move_type = MOVE_TYPE_DWELL;
//	sps.timer_period = _f_to_period(FREQUENCY_DWELL);
	sps.timer_ticks = (uint32_t)((microseconds/1000000) * FREQUENCY_DWELL);
}

/****************************************************************************************
 * UTILITIES
 * st_isbusy()			- return TRUE if motors are running or a dwell is running
 * st_set_microsteps()	- set microsteps in hardware
 *
 *	For now the microstep_mode is the same as the microsteps (1,2,4,8)
 *	This may change if microstep morphing is implemented.
 */
uint8_t st_isbusy()
{
	if (st.timer_ticks_downcount == 0) {
		return (false);
	} 
	return (true);
}

void st_set_microsteps(const uint8_t motor, const uint8_t microstep_mode)
{
/*
	if (microstep_mode == 8) {
		device.st_port[motor]->OUTSET = MICROSTEP_BIT_0_bm;
		device.st_port[motor]->OUTSET = MICROSTEP_BIT_1_bm;
	} else if (microstep_mode == 4) {
		device.st_port[motor]->OUTCLR = MICROSTEP_BIT_0_bm;
		device.st_port[motor]->OUTSET = MICROSTEP_BIT_1_bm;
	} else if (microstep_mode == 2) {
		device.st_port[motor]->OUTSET = MICROSTEP_BIT_0_bm;
		device.st_port[motor]->OUTCLR = MICROSTEP_BIT_1_bm;
	} else if (microstep_mode == 1) {
		device.st_port[motor]->OUTCLR = MICROSTEP_BIT_0_bm;
		device.st_port[motor]->OUTCLR = MICROSTEP_BIT_1_bm;
	}
*/
}