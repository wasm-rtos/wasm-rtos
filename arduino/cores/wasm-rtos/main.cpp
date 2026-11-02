/* A sketch runs as a program: setup() once, then loop() for as long as it runs. */
#include "Arduino.h"

void wr_arduino_after_loop(void);

int main(void)
{
    setup();
    for (;;)
    {
        loop();
        wr_arduino_after_loop();
    }
}
