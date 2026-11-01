#ifndef QOH99_INSTRUMENT_H
#define QOH99_INSTRUMENT_H

void qoh99_instr_on_frame(void);

void qoh99_instr_reset(void);

void qoh99_instr_dump(const char *reason);

void qoh99_instr_shutdown(void);

#endif
