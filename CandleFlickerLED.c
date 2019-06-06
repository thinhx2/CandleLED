/**
 * CandleFlickerLED.c forked from https://github.com/cpldcpu/CandleLEDhack.
 *
 * Emulates a candleflicker-LED on an AVR microcontroller.
 */

#include <avr/io.h>
#include <avr/interrupt.h>

/**
 * This is a 32-bit long LFSR. The point is to decrease repetition in the
 * sequences, but it takes a toll on performance (slightly) and on program/
 * memory size (see http://www.ece.cmu.edu/~koopman/lfsr/index.html on LFSRs).
 * 
 * Values are inverted so the LFSR also works with zero initialization.
 * 
 * (Ended up inlined in Assembly, because of the 0xFF = 1's complement
 * optimization).
 */
#define LFSR_FEEDBACK_TERM 0x7FFFF159

// Led connected to PB0
#define LEDDDR  DDRB
#define LEDPIN  PB0

// Select only the lowest byte (GCC ASM optim)
#define low_byte(x) ((uint8_t) x)

int main(void)
{
    cli();

    /**
     * CPU base frequency (fuses)     = 4.8 MHz
     * CPU clock division factor      = 4
     * CPU frequency                  = 1.2 MHz
     * 
     * Counter0 clock division factor = 8
     * Counter0 steps                 = 256 (8 bits)
     * Counter0 overflows in a frame  = 32
     * 
     * Hence:
     * PWM change frequency           = 18,31 Hz
     * PWM change period              = 54,61 ms
     * 
     * To avoid unintentional changes of clock frequency, a special write
     * procedure must be followed to change the CLKPS bits:
     * 1. Write the Clock Prescaler Change Enable (CLKPCE) bit to one and all 
     * other bits in CLKPR to zero.
     * 2. Within four cycles, write the desired value to CLKPS while writing a
     * zero to CLKPCE.
     */
    CLKPR = _BV(CLKPCE);
    CLKPR = _BV(CLKPS1);                // Set clk division factor to 4
	
    // Set output pin direction
    LEDDDR |= _BV(LEDPIN);              // LED is connected to PB0
    
    // Timer/Counter Control Register A
#ifdef __INVERTED_PWM
    TCCR0A = _BV(COM0A1) | _BV(COM0A0)  // Inverted PWM (0 at start, 1 on match)
#else
    TCCR0A = _BV(COM0A1)                // "Normal" PWM (1 at start, 0 on match)
#endif
           | _BV(WGM01)  | _BV(WGM00);  // Fast PWM mode 0x00-0xFF then overflow
    // Timer/Counter Control Register B
    TCCR0B = _BV(CS01);                 // Counter started, f/8
    // Timer/Counter Interrupt Mask Register
    TIMSK0 = _BV(TOIE0);                // Timer/Counter0 Overflow Int Enable
    OCR0A = 0;

    MCUCR = _BV(SE);                    // Sleep Enable, SM = 0 (Idle), PUD = ISC = 0.

    __asm__(
            "mov r20,r1" "\n\t"
            "mov r21,r1" "\n\t"
            "mov r22,r1" "\n\t"
            "mov r23,r1" "\n\t"
            "mov r24,r1"
            : /* no output registers */
            : /* no input registers */
            : "r20", "r21", "r22", "r23", "r24"
           );

    sei();
    
    while (1)
    {
        __asm__("sleep");
    }
}

/**
 * ISR triggered on Timer0 overflow. Overflow of the timer = frame counter
 * increment.
 * 
 * Naked function means that no prologue (registers pushed to stack) or epilogue
 * (register popped from stack) is generated by the compiler. Since this is
 * actually the only function ever called (after main is done setting up
 * interrupts), we don't really care about the registers state. We need to call
 * reti ourselves, though, since it's part of the epilogue.
 */
__attribute__((naked)) ISR(TIM0_OVF_vect)
{
    __asm__(
            /**
             * Increment frame counter.
             */
            "inc r24" "\n\t"            // FRAME_CTR++
            "andi r24,0x1F" "\n\t"      // FRAME_CTR &= 0x1F

            /**
             * Generate a new random brightness value at the bottom of each frame, and
             * if the number we've generated is deemed invalid, we retry up to three
             * times to make a new one (0b00000 + ~0x07 = 0b01000, 0b10000, 0b11000).
             * 
             * Bad values are those whose bits 2 and 3 (0b1100 = 0xC) are not set. These
             * values will be too low for our flicker to work.
             */
            "mov r31,r24" "\n\t"
            "tst r24" "\n\t"            // if (FRAME_CTR == 0) goto NEW_RAND
            "breq .LNEW_RAND" "\n\t"
            "andi r31,lo8(7)" "\n\t"    // if ((FRAME_CTR & 0x7) != 0) goto NEW_PWM
            "brne .LNEW_PWM" "\n\t"
            "mov r31,r20" "\n\t"
            "andi r31,lo8(12)" "\n\t"   // if ((RAND & 0xC) != 0) goto NEW_PWM
            "brne .LNEW_PWM" "\n"

        ".LNEW_RAND:" "\n\t"
            "mov r31,r20" "\n\t"
            "lsr r23" "\n\t"            // RAND >>= 1
            "ror r22" "\n\t"
            "ror r21" "\n\t"
            "ror r20" "\n\t"

            "sbrc r31,0" "\n\t"         // if !(old_RAND & 1) goto NEW_PWM
            "rjmp .LNEW_PWM" "\n\t"

            "ldi r31,0x59" "\n\t"       // RAND ^= LFSR_FEEDBACK_TERM
            "eor r20,r31" "\n\t"
            "ldi r31,0xF1" "\n\t"
            "eor r21,r31" "\n\t"
            //~ "ldi r31,0xFF" "\n\t"
            //~ "eor r22,r31" "\n\t"
            "com r22" "\n\t"
            "ldi r31,0x7F" "\n\t"
            "eor r23,r31" "\n"

        ".LNEW_PWM:" "\n\t"
            /**
             * Top of a frame (0x1F): set the new PWM value from the generated RAND.
             * 
             * We saturate the 5-bit random value to 4 bits so that 50% of the time,
             * the LED is full on.
             * 
             * The bit shift is here to fill the 8 bits of the PWM counter.
             */
            "cpi r24,0x1F" "\n\t"       // if (FRAME_CTR != 0x1F) reti;
            "brne .LRETI" "\n\t"

            "ldi r31,0xFF" "\n\t"
            "sbrc r20,4" "\n\t"         // if (RAND & 0x10) PWM = 0xFF;
            "rjmp .LSET_PWM" "\n\t"

            "mov r31,r20" "\n\t"        // else PWM = (low_byte(RAND) << 4) | 0xF
            "swap r31" "\n\t"
            "ori r31,0x0F" "\n"

        ".LSET_PWM:" "\n\t"
            "out %0,r31" "\n"           // OCR0A = PWM

        ".LRETI:" "\n\t"
            "reti"

            : /* no output registers */
            : "I" (_SFR_IO_ADDR(OCR0A))
            : "r20", "r21", "r22", "r23", "r24", "r31"
           );
}
