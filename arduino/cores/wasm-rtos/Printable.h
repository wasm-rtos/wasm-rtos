#ifndef WASM_RTOS_ARDUINO_PRINTABLE_H
#define WASM_RTOS_ARDUINO_PRINTABLE_H

class Print;

/* Something that prints itself: print(object) calls printTo. */
class Printable
{
public:
    virtual ~Printable() {}
    virtual size_t printTo(Print& out) const = 0;
};

#endif
